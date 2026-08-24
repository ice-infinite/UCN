#include "test_support.h"

#include <stdlib.h>
#include <string.h>

#include "cluster_test_fixture.h"
#include "ucn/ucn_cluster.h"
#include "ucn/ucn_cluster_epoch.h"
#include "ucn/ucn_cluster_persist.h"

#include "ucn_cluster_internal.h"

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

typedef struct cluster_test_id_provider {
    uint32_t ids[32];
    ucn_cluster_id_request_t requests[32];
    size_t supplied;
    size_t count;
    ucn_result_t result;
} cluster_test_id_provider_t;

typedef struct cluster_test_persistence_probe {
    uint32_t load_calls;
    uint32_t submit_calls;
    ucn_result_t load_result;
    ucn_cluster_persist_load_result_t loaded;
} cluster_test_persistence_probe_t;

static ucn_result_t cluster_test_persist_load(
    void *context,
    ucn_cluster_persist_load_result_t *result)
{
    cluster_test_persistence_probe_t *probe =
        (cluster_test_persistence_probe_t *)context;

    if (probe == NULL || result == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    probe->load_calls++;
    if (probe->load_result != UCN_OK) {
        return probe->load_result;
    }
    *result = probe->loaded;
    return UCN_OK;
}

static ucn_cluster_persist_completion_t cluster_test_persist_submit(
    void *context,
    const ucn_cluster_persist_request_t *request)
{
    cluster_test_persistence_probe_t *probe =
        (cluster_test_persistence_probe_t *)context;
    ucn_cluster_persist_completion_t completion;

    completion.state = request == NULL || probe == NULL ||
                       !ucn_cluster_persist_request_is_valid(request) ?
                           UCN_CLUSTER_PERSIST_FAILED :
                           UCN_CLUSTER_PERSIST_COMMITTED;
    completion.token = UCN_CLUSTER_PERSIST_TOKEN_NONE;
    completion.failure = completion.state == UCN_CLUSTER_PERSIST_FAILED ?
                             UCN_ERR_ARGUMENT : UCN_OK;
    if (completion.state == UCN_CLUSTER_PERSIST_COMMITTED) {
        probe->submit_calls++;
        probe->loaded.state = UCN_CLUSTER_PERSIST_LOAD_READY;
        probe->loaded.snapshot = request->next_state;
    }
    return completion;
}

static void cluster_test_persist_set_factory(
    cluster_test_persistence_probe_t *probe)
{
    (void)memset(&probe->loaded, 0, sizeof(probe->loaded));
    probe->load_result = UCN_OK;
    probe->loaded.state = UCN_CLUSTER_PERSIST_LOAD_FACTORY_EMPTY;
}

static bool cluster_test_bytes_are_zero(const void *input, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)input;
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static ucn_result_t cluster_test_make_id(
    void *context,
    const ucn_cluster_id_request_t *request,
    uint32_t *cluster_id)
{
    cluster_test_id_provider_t *provider =
        (cluster_test_id_provider_t *)context;

    if (provider == NULL || request == NULL || cluster_id == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (provider->result != UCN_OK) {
        return provider->result;
    }
    if (provider->count >= provider->supplied) {
        return UCN_ERR_NO_SPACE;
    }
    provider->requests[provider->count] = *request;
    *cluster_id = provider->ids[provider->count];
    provider->count++;
    return UCN_OK;
}

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
    case UCN_CLUSTER_ROLE_TERM_CONFLICT:
        return UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT;
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
        config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
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
    /* CLV2-01-04d.3: the migrated READY handler validates shadow == the
     * derived old phase (HEAD_BACKUP_SYNCING: node selected, sweep done,
     * snapshot in flight) before the STABLE transition, so the manual
     * staging must align the mirror too. */
    head->cluster.backup_assign_pending = false;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;

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

/* CLV2-01-04d.3: handle_backup_ready() is wired onto cluster_transition()
 * - the READY is the SYNCING/ASSIGNING -> HEAD_STABLE transition, run
 * BEFORE the ready=true write and fail-closed.  (a) a SYNCING Head
 * receives BACKUP_READY -> shadow==STABLE, reason==SNAPSHOT_READY;
 * (b) an ASSIGNING Head (assign_pending) receives the same epoch ->
 * identical result, old_phase==HEAD_BACKUP_ASSIGNING; (c) a shadow
 * desync fails closed with UCN_ERR_STATE and ready stays false. */
static int cluster_test_backup_ready_transition(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_cluster_t *c;
    ucn_cluster_t before;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];
    c = &head->cluster;
    network.now_ms = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_READY;
    message.role = UCN_CLUSTER_ROLE_BACKUP;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.membership_sequence = 3U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);

    /* (a) SYNCING Head: selected Backup, sweep complete, snapshot in
     * flight (assign_pending false, ready false). */
    c->role = UCN_CLUSTER_ROLE_HEAD;
    c->cluster_id = 1U;
    c->term = 1U;
    c->head_node_id = head->node_id;
    c->backup_node_id = network.nodes[1].node_id;
    c->backup_generation = 1U;
    c->membership_sequence = 3U;
    c->backup_ready = false;
    c->backup_assign_pending = false;
    c->shadow_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
    TEST_ASSERT(ucn_cluster_receive(c, network.nodes[1].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_HEAD_STABLE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_SNAPSHOT_READY);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->backup_ready == true);
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_HEAD_STABLE);

    /* (b) ASSIGNING Head: same-epoch READY while the assignment sweep is
     * still pending - old_phase derives HEAD_BACKUP_ASSIGNING. */
    c->role = UCN_CLUSTER_ROLE_HEAD;
    c->cluster_id = 1U;
    c->term = 1U;
    c->head_node_id = head->node_id;
    c->backup_node_id = network.nodes[1].node_id;
    c->backup_generation = 1U;
    c->membership_sequence = 3U;
    c->backup_ready = false;
    c->backup_assign_pending = true;
    c->shadow_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING;
    TEST_ASSERT(ucn_cluster_receive(c, network.nodes[1].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_HEAD_STABLE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_SNAPSHOT_READY);
    TEST_ASSERT(c->backup_ready == true);
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_HEAD_STABLE);

    /* (c) fail-closed: the shadow claims DETACHED_OBSERVE while the
     * legacy state derives SYNCING - the STABLE transition rejects with
     * UCN_ERR_STATE and ready stays false (no phase-relevant write). */
    c->role = UCN_CLUSTER_ROLE_HEAD;
    c->cluster_id = 1U;
    c->term = 1U;
    c->head_node_id = head->node_id;
    c->backup_node_id = network.nodes[1].node_id;
    c->backup_generation = 1U;
    c->membership_sequence = 3U;
    c->backup_ready = false;
    c->backup_assign_pending = false;
    c->shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE; /* desync */
    /* Rejection tests silence the Debug assert knob (framework
     * convention, cluster_transition_assert_enabled) so the release
     * behaviour - UCN_ERR_STATE with ZERO writes - can be verified. */
    ucn_cluster_test_transition_asserts_set(false);
    before = *c;
    TEST_ASSERT(ucn_cluster_receive(c, network.nodes[1].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_ERR_STATE);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->backup_assign_pending == false);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_HEAD);
    TEST_ASSERT(c->backup_node_id == network.nodes[1].node_id);
    TEST_ASSERT(c->membership_sequence == before.membership_sequence);
    /* The transition never committed: no STABLE shadow, no READY reason. */
    TEST_ASSERT(c->shadow_phase != UCN_CLUSTER_PHASE_HEAD_STABLE);
    TEST_ASSERT(c->transition_reason != UCN_CLUSTER_REASON_SNAPSHOT_READY);
    /* The end-of-RX mirror sync re-aligns shadow to the UNCHANGED legacy
     * state (SYNCING) - exactly the CLV2-01-02 behaviour for a rejected
     * message; the legacy state itself never derived STABLE. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
    ucn_cluster_test_transition_asserts_set(true);
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
    backup->cluster.primary_members.slots[0].occupied = true;
    backup->cluster.primary_members.slots[0].node_id = backup->node_id;
    backup->cluster.primary_members.slots[1].occupied = true;
    backup->cluster.primary_members.slots[1].node_id = member->node_id;
    backup->cluster.backup_primary_deadline_ms = 1U;
    backup->cluster.backup_missed_heartbeats = UCN_CLUSTER_BACKUP_MISS_LIMIT;
    backup->cluster.backup_primary_lease_deadline_ms = 1U;
    network.now_ms = 100U;
    /* CLV2-01-04e.3: this test constructs the BACKUP_READY legacy state
     * directly, so the shadow mirror must be seeded to the same derived
     * phase (the real FSM reaches READY through stepped transitions and
     * keeps shadow == derive).  start_takeover() commits the transition
     * UNCONDITIONALLY; the validate gate would reject a stale mirror. */
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;

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
/* CLV2-M03 (03-04): the real RX pre-dispatch sends a same-Cluster higher-Term
 * Head offer through process_higher_authority().  That common path commits
 * BACKUP_READY / BACKUP_SYNCING / BACKUP_TAKEOVER -> JOIN_PENDING with
 * HIGHER_AUTHORITY before its original Backup cleanup and join payload.  A
 * shadow/legacy desync fails closed with zero site writes; the Backup retains
 * its state and a later valid offer may still be accepted. */
static int cluster_test_takeover_interrupted_by_newer_head(void)
{
    cluster_test_network_t network;
    cluster_test_network_t net2;
    cluster_test_network_t net3;
    cluster_test_node_t *backup;
    cluster_test_node_t *newer_head;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    uint32_t baseline_count;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    backup = &network.nodes[1];
    newer_head = &network.nodes[0];
    backup->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
    backup->cluster.cluster_id = 1U;
    backup->cluster.term = 1U;
    backup->cluster.head_node_id = newer_head->node_id;
    backup->cluster.backup_primary_node_id = newer_head->node_id;
    backup->cluster.backup_generation = 1U;
    /* M01.0.2: takeover_active && backup_syncing is REACHABLE (a late same-generation
     * Type12 during takeover); the higher-Term offer must never be rejected for phase
     * reasons - the derive stays BACKUP_TAKEOVER and the transition must commit. */
    backup->cluster.backup_takeover_active = true;
    backup->cluster.backup_syncing = true;
    backup->cluster.backup_primary_lease_deadline_ms = 1U;
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
    backup->cluster.transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    backup->cluster.shadow_transition_count = 0U;
    network.now_ms = 50U;
    TEST_ASSERT(test_derive_phase(&backup->cluster, 50U) ==
                UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);

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
    TEST_ASSERT(backup->cluster.shadow_transition_count == 0U);

    /* The same Cluster at a higher term interrupts and joins.  CLV2-M03
     * (03-04) routes it through the global HIGHER_AUTHORITY transition FIRST (fail
     * closed), then the site's writes run in order: takeover=false, backup_clear_sync()
     * (identity cleared, epoch reset via set_detached), begin_join() (pending_* re-populated
     * from the candidate). */
    baseline_count = backup->cluster.shadow_transition_count;
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
    TEST_ASSERT(backup->cluster.shadow_phase == UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(backup->cluster.transition_reason ==
                UCN_CLUSTER_REASON_HIGHER_AUTHORITY);
    TEST_ASSERT(backup->cluster.shadow_transition_count ==
                baseline_count + 1U);
    /* backup_clear_sync() + set_detached() site effects in original order. */
    TEST_ASSERT(backup->cluster.backup_syncing == false);
    TEST_ASSERT(backup->cluster.backup_ready == false);
    TEST_ASSERT(backup->cluster.backup_primary_node_id == 0U);
    TEST_ASSERT(backup->cluster.backup_generation == 0U);
    TEST_ASSERT(backup->cluster.membership_sequence == 0U);
    TEST_ASSERT(backup->cluster.backup_primary_deadline_ms == 0U);
    TEST_ASSERT(backup->cluster.backup_missed_heartbeats == 0U);
    TEST_ASSERT(backup->cluster.cluster_id == 0U);
    TEST_ASSERT(backup->cluster.term == 0U);
    TEST_ASSERT(backup->cluster.head_node_id == 0U);
    TEST_ASSERT(backup->cluster.current_head_score == 0U);
    TEST_ASSERT(backup->cluster.known_backup_node_id == 0U);
    TEST_ASSERT(backup->cluster.known_backup_generation == 0U);
    TEST_ASSERT(backup->cluster.head_lease_expires_at_ms == 0U);
    TEST_ASSERT(backup->cluster.head_grace_deadline_ms == 0U);
    TEST_ASSERT(backup->cluster.election_deadline_ms == 0U);
    /* begin_join() re-populates the pending fields from the candidate. */
    TEST_ASSERT(backup->cluster.pending_head_node_id == newer_head->node_id);
    TEST_ASSERT(backup->cluster.pending_cluster_id == 1U);
    TEST_ASSERT(backup->cluster.pending_term == 2U);
    TEST_ASSERT(backup->cluster.pending_head_score == 9000U);
    TEST_ASSERT(backup->cluster.role_since_ms == 50U);
    TEST_ASSERT(backup->cluster.next_join_retry_ms == 50U);
    TEST_ASSERT(backup->cluster.recovery_eligible == false);
    TEST_ASSERT(backup->cluster.recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(test_derive_phase(&backup->cluster, 50U) ==
                UCN_CLUSTER_PHASE_JOIN_PENDING);

    /* Scenario A: a READY Backup (no takeover) accepts the same higher-Term same-Cluster
     * Head offer -> BACKUP_READY -> JOIN_PENDING (HIGHER_AUTHORITY), full backup identity
     * cleared, takeover stays clear. */
    TEST_ASSERT(cluster_test_network_init(&net2, 3U) == 0);
    net2.now_ms = 50U;
    net2.nodes[1].cluster.role = UCN_CLUSTER_ROLE_BACKUP;
    net2.nodes[1].cluster.cluster_id = 1U;
    net2.nodes[1].cluster.term = 1U;
    net2.nodes[1].cluster.head_node_id = 1U;
    net2.nodes[1].cluster.backup_primary_node_id = 1U;
    net2.nodes[1].cluster.backup_generation = 7U;
    net2.nodes[1].cluster.backup_ready = true;
    net2.nodes[1].cluster.backup_syncing = false;
    net2.nodes[1].cluster.backup_takeover_active = false;
    net2.nodes[1].cluster.backup_primary_deadline_ms = 200U;
    net2.nodes[1].cluster.membership_sequence = 4U;
    net2.nodes[1].cluster.backup_missed_heartbeats = 2U;
    net2.nodes[1].cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;
    net2.nodes[1].cluster.transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    net2.nodes[1].cluster.shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(&net2.nodes[1].cluster, 50U) ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 2U;
    message.head_node_id = 1U;
    message.head_score = 9000U;
    message.available_capacity = 2U;
    message.lease_ms = 8000U;
    message.nonce = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&net2.nodes[1].cluster, 1U, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(net2.nodes[1].cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(net2.nodes[1].cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(net2.nodes[1].cluster.transition_reason ==
                UCN_CLUSTER_REASON_HIGHER_AUTHORITY);
    TEST_ASSERT(net2.nodes[1].cluster.shadow_transition_count == 1U);
    TEST_ASSERT(net2.nodes[1].cluster.backup_takeover_active == false);
    TEST_ASSERT(net2.nodes[1].cluster.backup_ready == false);
    TEST_ASSERT(net2.nodes[1].cluster.backup_syncing == false);
    TEST_ASSERT(net2.nodes[1].cluster.backup_primary_node_id == 0U);
    TEST_ASSERT(net2.nodes[1].cluster.backup_generation == 0U);
    TEST_ASSERT(net2.nodes[1].cluster.membership_sequence == 0U);
    TEST_ASSERT(net2.nodes[1].cluster.backup_primary_deadline_ms == 0U);
    TEST_ASSERT(net2.nodes[1].cluster.backup_missed_heartbeats == 0U);
    TEST_ASSERT(net2.nodes[1].cluster.pending_head_node_id == 1U);
    TEST_ASSERT(net2.nodes[1].cluster.pending_cluster_id == 1U);
    TEST_ASSERT(net2.nodes[1].cluster.pending_term == 2U);
    TEST_ASSERT(net2.nodes[1].cluster.pending_head_score == 9000U);
    TEST_ASSERT(net2.nodes[1].cluster.role_since_ms == 50U);
    TEST_ASSERT(net2.nodes[1].cluster.next_join_retry_ms == 50U);
    TEST_ASSERT(net2.nodes[1].cluster.recovery_eligible == false);
    TEST_ASSERT(net2.nodes[1].cluster.recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(test_derive_phase(&net2.nodes[1].cluster, 50U) ==
                UCN_CLUSTER_PHASE_JOIN_PENDING);

    /* Scenario B (fail closed): a shadow/legacy desync (stale shadow BACKUP_TAKEOVER while
     * the legacy derives BACKUP_READY) rejects the transition - ZERO site writes: no
     * takeover clear, no backup identity clear, no join (pending_* untouched); the end-of-RX
     * shadow sync merely reconciles the mirror to BACKUP_READY. */
    TEST_ASSERT(cluster_test_network_init(&net3, 3U) == 0);
    net3.now_ms = 50U;
    net3.nodes[1].cluster.role = UCN_CLUSTER_ROLE_BACKUP;
    net3.nodes[1].cluster.cluster_id = 1U;
    net3.nodes[1].cluster.term = 1U;
    net3.nodes[1].cluster.head_node_id = 1U;
    net3.nodes[1].cluster.backup_primary_node_id = 1U;
    net3.nodes[1].cluster.backup_generation = 7U;
    net3.nodes[1].cluster.backup_ready = true;
    net3.nodes[1].cluster.backup_syncing = false;
    net3.nodes[1].cluster.backup_takeover_active = false;
    net3.nodes[1].cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
    net3.nodes[1].cluster.transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    net3.nodes[1].cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 2U;
    message.head_node_id = 1U;
    message.head_score = 9000U;
    message.available_capacity = 2U;
    message.lease_ms = 8000U;
    message.nonce = 9U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    ucn_cluster_test_transition_asserts_set(false);
    TEST_ASSERT(ucn_cluster_receive(&net3.nodes[1].cluster, 1U, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(net3.nodes[1].cluster.role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(net3.nodes[1].cluster.shadow_phase !=
                UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(net3.nodes[1].cluster.transition_reason !=
                UCN_CLUSTER_REASON_HIGHER_AUTHORITY);
    TEST_ASSERT(net3.nodes[1].cluster.shadow_transition_count == 1U);
    TEST_ASSERT(net3.nodes[1].cluster.backup_ready == true);
    TEST_ASSERT(net3.nodes[1].cluster.backup_syncing == false);
    TEST_ASSERT(net3.nodes[1].cluster.backup_takeover_active == false);
    TEST_ASSERT(net3.nodes[1].cluster.backup_primary_node_id == 1U);
    TEST_ASSERT(net3.nodes[1].cluster.backup_generation == 7U);
    TEST_ASSERT(net3.nodes[1].cluster.cluster_id == 1U);
    TEST_ASSERT(net3.nodes[1].cluster.term == 1U);
    TEST_ASSERT(net3.nodes[1].cluster.head_node_id == 1U);
    TEST_ASSERT(net3.nodes[1].cluster.pending_head_node_id == 0U);
    TEST_ASSERT(net3.nodes[1].cluster.pending_cluster_id == 0U);
    TEST_ASSERT(net3.nodes[1].cluster.pending_term == 0U);
    TEST_ASSERT(net3.nodes[1].cluster.pending_head_score == 0U);
    TEST_ASSERT(net3.nodes[1].cluster.recovery_eligible == false);
    return 0;
}


/* C07.7 P1: available_capacity == 0 only gates new JOINs; it must not
 * block a higher-Term Head from converging a full Head onto it.
 *
 * CLV2-M03 (03-03, human-audited): the converging Head offer must be
 * SAME-cluster.  The pre-03-03 test used cluster_id = 2 (foreign) to
 * prove capacity independence, but that pinned the exact behavior 03-03
 * deletes - a foreign Cluster's higher Term is never authority, so a
 * full (capacity 0) foreign Head must NOT pull a Cluster-A Head down.
 * The P1 concern survives unchanged: capacity 0 must not block the
 * legitimate SAME-cluster higher-Term convergence. */
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
    /* CLV2-01-04d.6: the HEAD_NO_BACKUP -> STEPPING_DOWN transition now
     * validates shadow == old_phase, so the fixture must keep the shadow
     * mirror aligned with the legacy derive (role == HEAD, no Backup). */
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;

    /* FOREIGN offer (Cluster B, term 2 > local 1, capacity 0): the
     * higher Term is NOT comparable across clusters - the Head must stay
     * put even though the foreign Head is full and "newer". */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 2U;   /* foreign Cluster B */
    message.term = 2U;         /* higher NUMBER, but NOT comparable */
    message.head_node_id = network.nodes[1].node_id;
    message.head_score = 6000U;
    message.available_capacity = 0U; /* full */
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    network.now_ms = 60U;
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, network.nodes[1].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_HEAD);
    TEST_ASSERT(head->cluster.shadow_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    TEST_ASSERT(head->cluster.shadow_transition_count == 0U);

    /* SAME-cluster offer (Cluster A, term 2 > local 1, capacity 0): this
     * IS legitimate higher authority - the full Head converges onto it
     * (the C07.7 P1 point: capacity 0 never blocks epoch convergence). */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;   /* same Cluster A */
    message.term = 2U;         /* newer generation */
    message.head_node_id = network.nodes[1].node_id;
    message.head_score = 6000U; /* lower score but newer term */
    message.available_capacity = 0U; /* full */
    message.lease_ms = 8000U;
    message.nonce = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, network.nodes[1].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_STEPPING_DOWN);
    TEST_ASSERT(head->cluster.shadow_phase == UCN_CLUSTER_PHASE_STEPPING_DOWN);
    TEST_ASSERT(head->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(head->cluster.pending_head_node_id == network.nodes[1].node_id);
    TEST_ASSERT(head->cluster.pending_cluster_id == 1U);
    TEST_ASSERT(head->cluster.pending_term == 2U);
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

    /* CLV2-03-R03: Type 12 receiver ordering uses the same bounded serial
     * domain as the sender.  Zero and values beyond the rotation threshold
     * are never interpreted as a successor, including a hostile raw-wrap
     * attempt while the local mirror has reached its terminal value. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 2U;
    message.flags = UCN_CLUSTER_FLAG_SYNC_BEGIN;
    message.membership_sequence = 0U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_ERR_REPLAY);
    TEST_ASSERT(backup->cluster.membership_sequence == 5U);
    message.membership_sequence = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_ERR_REPLAY);
    TEST_ASSERT(backup->cluster.membership_sequence == 5U);
    backup->cluster.membership_sequence =
        UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    message.flags = UCN_CLUSTER_FLAG_SYNC_DELTA;
    message.membership_sequence = 0U;
    message.member_node_id = network.nodes[2].node_id;
    message.member_nonce = 17U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_ERR_REPLAY);
    TEST_ASSERT(backup->cluster.membership_sequence ==
                UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD);
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
    /* CLV2-01-04e.7: the manual staging must align the shadow mirror with
     * the derived BACKUP_READY phase - the migrated DELTA-gap handler now
     * commits an explicit READY -> SYNCING transition (RESYNC_STARTED) and
     * validates shadow == old before it (shadow-guard closure, exactly the
     * d.7 MAJOR 1C staging fix at cluster_test_backup_reject_switches_
     * candidate()). */
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;
    backup->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    backup->cluster.shadow_transition_count = 0U;
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
    /* CLV2-01-04d.7 (MAJOR 1C): the reject path now transitions the
     * HEAD_BACKUP_* -> HEAD_NO_BACKUP phase explicitly, so the shadow must
     * be synced to the setup state (a real head has a synced shadow at
     * every step/RX boundary). */
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
    head->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    head->cluster.shadow_transition_count = 0U;
    /* Mirror: node2 (rejected, score 3000) and node3 (score 2000). */
    head->cluster.primary_members.slots[0].occupied = true;
    head->cluster.primary_members.slots[0].node_id = rejected->node_id;
    head->cluster.primary_members.slots[1].occupied = true;
    head->cluster.primary_members.slots[1].node_id = network.nodes[2].node_id;
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
    /* CLV2-01-04c.5: the migrated MEMBER stepdown path validates
     * shadow == old_phase (MEMBER_ACTIVE) fail-closed - align the
     * mirror, exactly as a real join flow leaves it after the
     * end-of-step/RX shadow sync. */
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
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

/* CLV2-01-04e.1: handle_backup_assign() is wired onto cluster_transition()
 * - the assignment IS the MEMBER_ACTIVE / MEMBER_TAKEOVER_GRACE /
 * JOIN_PENDING -> BACKUP_SYNCING transition (reason BACKUP_ASSIGNED), run
 * UNCONDITIONALLY (no shadow guard) BEFORE any primary/generation/mirror
 * write and fail closed.  (a) MEMBER_ACTIVE -> shadow==BACKUP_SYNCING,
 * reason==BACKUP_ASSIGNED, primary/gen/mirror fields set; (a2) a MEMBER
 * with the grace deadline armed -> same via the GRACE old phase; (b)
 * JOIN_PENDING (pre-assigned path) -> same; (c) a shadow desync fails
 * closed with UCN_ERR_STATE and primary/generation/mirror are NOT
 * written (mirror-symmetric to the d.3 READY rejection test). */
static int cluster_test_backup_assign_transition(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_cluster_t *c;
    ucn_cluster_t before;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 0U;
    node = &network.nodes[1]; /* head-capable, so BACKUP_ASSIGN(self) works */
    c = &node->cluster;

    /* Encoded BACKUP_ASSIGN(self) from the Head (node 0). */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_ASSIGN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 5U;
    message.head_node_id = network.nodes[0].node_id;
    message.sync_token = node->node_id;
    message.backup_generation = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);

    /* (a) MEMBER_ACTIVE receives BACKUP_ASSIGN(self): the transition is
     * committed BEFORE the primary/generation/mirror writes and every
     * site effect lands in order. */
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    node->cluster.cluster_id = 1U;
    node->cluster.term = 5U;
    node->cluster.head_node_id = network.nodes[0].node_id;
    node->cluster.config.head_capable = true;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    c->primary_members.slots[0].occupied = true; /* stale mirror must be cleared */
    c->primary_members.slots[0].node_id = 3U;
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(ucn_cluster_receive(c, network.nodes[0].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_BACKUP_ASSIGNED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(c->backup_syncing == true);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->backup_primary_node_id == network.nodes[0].node_id);
    TEST_ASSERT(c->backup_generation == 1U);
    TEST_ASSERT(c->membership_sequence == 0U);
    TEST_ASSERT(c->backup_primary_deadline_ms != 0U);
    TEST_ASSERT(c->backup_primary_lease_deadline_ms != 0U);
    TEST_ASSERT(c->backup_missed_heartbeats == 0U);
    TEST_ASSERT(c->primary_members.slots[0].occupied == false); /* cleared */
    TEST_ASSERT(c->known_backup_node_id == node->node_id);
    TEST_ASSERT(c->known_backup_generation == 1U);
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);

    /* (a2) a MEMBER in TAKEOVER_GRACE (grace deadline armed) receives it:
     * old_phase derives MEMBER_TAKEOVER_GRACE from the PRE-CALL legacy
     * state. */
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    node->cluster.cluster_id = 1U;
    node->cluster.term = 5U;
    node->cluster.head_node_id = network.nodes[0].node_id;
    node->cluster.config.head_capable = true;
    node->cluster.head_grace_deadline_ms =
        ucn_deadline_from_now(network.now_ms, 40U);
    /* Simulate a former takeover whose lifecycle had already returned this
     * node to Member before a new self-assignment arrived. */
    node->cluster.backup_takeover_active = true;
    node->cluster.backup_takeover_ack_count = 2U;
    node->cluster.backup_takeover_acked = UINT32_C(3);
    node->cluster.backup_takeover_deadline_ms = 200U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE;
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE);
    TEST_ASSERT(ucn_cluster_receive(c, network.nodes[0].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_BACKUP_ASSIGNED);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(c->backup_primary_node_id == network.nodes[0].node_id);
    TEST_ASSERT(c->backup_generation == 1U);
    TEST_ASSERT(c->backup_takeover_active == false);
    TEST_ASSERT(c->backup_takeover_ack_count == 0U);
    TEST_ASSERT(c->backup_takeover_acked == 0U);
    TEST_ASSERT(c->backup_takeover_deadline_ms == 0U);
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);

    /* (b) JOIN_PENDING (pre-assigned join path) receives it: same
     * transition, shadow stays BACKUP_SYNCING with the identity set. */
    node->cluster.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    node->cluster.cluster_id = 1U;
    node->cluster.term = 5U;
    node->cluster.head_node_id = network.nodes[0].node_id;
    node->cluster.pending_head_node_id = network.nodes[0].node_id;
    node->cluster.pending_cluster_id = 1U;
    node->cluster.pending_term = 5U;
    node->cluster.pending_join_nonce = 9U;
    node->cluster.config.head_capable = true;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_JOIN_PENDING;
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(ucn_cluster_receive(c, network.nodes[0].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_BACKUP_ASSIGNED);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(c->backup_syncing == true);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->backup_primary_node_id == network.nodes[0].node_id);
    TEST_ASSERT(c->backup_generation == 1U);
    TEST_ASSERT(c->known_backup_node_id == node->node_id);
    TEST_ASSERT(c->known_backup_generation == 1U);
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);

    /* (b2) CLV2-03-R04: a physically valid Head must not use BACKUP_ASSIGN
     * as an alternate cross-epoch switch path.  The pending/shared record is
     * untouched when its (cluster_id, term) does not match the Member. */
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    node->cluster.cluster_id = 1U;
    node->cluster.term = 5U;
    node->cluster.head_node_id = network.nodes[0].node_id;
    node->cluster.head_grace_deadline_ms = 0U;
    node->cluster.config.head_capable = true;
    node->cluster.known_backup_node_id = UINT32_C(0xAA);
    node->cluster.known_backup_generation = 9U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    message.cluster_id = 2U;
    message.term = 99U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, network.nodes[0].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_ERR_REPLAY);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->cluster_id == 1U && c->term == 5U);
    TEST_ASSERT(c->known_backup_node_id == UINT32_C(0xAA));
    TEST_ASSERT(c->known_backup_generation == 9U);
    message.cluster_id = 1U;
    message.term = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);

    /* (c) fail-closed: the shadow claims DETACHED_OBSERVE while the legacy
     * state derives MEMBER_ACTIVE - the BACKUP_SYNCING transition rejects
     * with UCN_ERR_STATE and primary/generation/mirror are NOT written.
     * The mirror/identity fields the earlier sub-cases committed are
     * explicitly reset first so the fail-closed assertions below prove
     * the rejected transition itself wrote NOTHING. */
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    node->cluster.cluster_id = 1U;
    node->cluster.term = 5U;
    node->cluster.head_node_id = network.nodes[0].node_id;
    node->cluster.config.head_capable = true;
    node->cluster.head_grace_deadline_ms = 0U;
    node->cluster.backup_primary_node_id = 0U;
    node->cluster.backup_generation = 0U;
    node->cluster.backup_syncing = false;
    node->cluster.backup_ready = false;
    node->cluster.backup_takeover_active = false;
    node->cluster.membership_sequence = 0U;
    node->cluster.backup_primary_deadline_ms = 0U;
    node->cluster.backup_primary_lease_deadline_ms = 0U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE; /* desync */
    /* Rejection tests silence the Debug assert knob (framework
     * convention, cluster_transition_assert_enabled) so the release
     * behaviour - UCN_ERR_STATE with ZERO writes - can be verified. */
    ucn_cluster_test_transition_asserts_set(false);
    before = *c;
    TEST_ASSERT(ucn_cluster_receive(c, network.nodes[0].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_ERR_STATE);
    /* The transition never committed: no primary/gen/mirror write. */
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->backup_primary_node_id == 0U);
    TEST_ASSERT(c->backup_generation == 0U);
    TEST_ASSERT(c->backup_syncing == false);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->backup_primary_deadline_ms == before.backup_primary_deadline_ms);
    TEST_ASSERT(c->backup_primary_lease_deadline_ms ==
                before.backup_primary_lease_deadline_ms);
    TEST_ASSERT(c->membership_sequence == before.membership_sequence);
    TEST_ASSERT(c->known_backup_node_id == before.known_backup_node_id);
    TEST_ASSERT(c->known_backup_generation == before.known_backup_generation);
    TEST_ASSERT(c->shadow_phase != UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(c->transition_reason != UCN_CLUSTER_REASON_BACKUP_ASSIGNED);
    /* The end-of-RX mirror sync re-aligns shadow to the UNCHANGED legacy
     * state (MEMBER_ACTIVE) - exactly the CLV2-01-02 behaviour for a
     * rejected message; the legacy state itself never derived SYNCING. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    ucn_cluster_test_transition_asserts_set(true);
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

    /* (c) CLV2-01-04b.6 (human MAJOR): a JOIN_PENDING node whose shadow is
     * deliberately desynced from legacy (shadow=DETACHED_OBSERVE while
     * role=JOIN_PENDING derives JOIN_PENDING) makes the transition's
     * shadow==old_phase check fail (the FIRST validation - the derive
     * assert is never reached).  The transition must be rejected
     * fail-closed and the anti-replay fence must NOT be consumed: no
     * half-commit. */
    node->cluster.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    node->cluster.cluster_id = 1U;
    node->cluster.term = 5U;
    node->cluster.head_node_id = network.nodes[0].node_id;
    node->cluster.pending_head_node_id = network.nodes[0].node_id;
    node->cluster.pending_cluster_id = 1U;
    node->cluster.pending_term = 5U;
    node->cluster.last_stepdown_nonce = 1U;
    ucn_cluster_test_transition_asserts_set(false); /* survive the knob assert */
    {
        uint32_t before_nonce = node->cluster.last_stepdown_nonce;
        ucn_node_id_t before_pending_head =
            node->cluster.pending_head_node_id;
        uint32_t before_pending_cluster =
            node->cluster.pending_cluster_id;
        uint32_t before_pending_term = node->cluster.pending_term;

        (void)memset(&message, 0, sizeof(message));
        message.type = UCN_CLUSTER_MSG_HEAD_STEPDOWN;
        message.role = UCN_CLUSTER_ROLE_HEAD;
        message.cluster_id = 1U;
        message.term = 5U;
        message.head_node_id = network.nodes[0].node_id;
        message.lease_ms = 8000U;
        message.nonce = 2U; /* fresh: strictly greater than the fence */
        TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        TEST_ASSERT(ucn_cluster_receive(&node->cluster,
                                        network.nodes[0].node_id,
                                        true, encoded, sizeof(encoded)) ==
                    UCN_ERR_STATE);
        /* THE fix: the fence is NOT consumed by a rejected transition. */
        TEST_ASSERT(node->cluster.last_stepdown_nonce == before_nonce);
        /* No partial commit: the legacy detach never ran. */
        TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
        TEST_ASSERT(node->cluster.pending_head_node_id == before_pending_head);
        TEST_ASSERT(node->cluster.pending_cluster_id == before_pending_cluster);
        TEST_ASSERT(node->cluster.pending_term == before_pending_term);
        /* The phase did NOT migrate: the end-of-RX shadow sync re-aligns
         * the mirror to the UNCHANGED legacy (derive==JOIN_PENDING), so
         * shadow returns to JOIN_PENDING - it never advanced to
         * DETACHED_OBSERVE and no STEPDOWN_ORDERED was recorded. */
        TEST_ASSERT(node->cluster.shadow_phase ==
                    UCN_CLUSTER_PHASE_JOIN_PENDING);
        TEST_ASSERT(node->cluster.transition_reason !=
                    UCN_CLUSTER_REASON_STEPDOWN_ORDERED);
    }
    ucn_cluster_test_transition_asserts_set(true);
    return 0;
}

/* CLV2-01-04c.5: the MEMBER sub-branch of the HEAD_STEPDOWN handler now
 * routes through the single transition entry point with the b.6
 * fail-closed discipline: the old phase is derived from the PRE-CALL
 * state (role==MEMBER: an armed grace deadline means MEMBER_TAKEOVER_GRACE,
 * otherwise MEMBER_ACTIVE), the transition runs FIRST, and the anti-replay
 * fence is consumed + set_detached() runs ONLY on success. */
static int cluster_test_member_stepdown_shadow_transition(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 0U;
    node = &network.nodes[2];

    /* (a) A MEMBER_ACTIVE node receives a HEAD_STEPDOWN of the CURRENT
     * epoch with a fresh nonce: MEMBER_ACTIVE -> DETACHED_OBSERVE through
     * the entry point with the EXPLICIT STEPDOWN_ORDERED reason (never
     * the RESET BEST-EFFORT fallback), the fence advances, and the legacy
     * detach site effects still run in their original order. */
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    node->cluster.cluster_id = 1U;
    node->cluster.term = 5U;
    node->cluster.head_node_id = network.nodes[0].node_id;
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
    TEST_ASSERT(node->cluster.shadow_transition_count == 1U);
    /* fence advanced; the site-side detach still ran (nonce then
     * set_detached(), in original order) */
    TEST_ASSERT(node->cluster.last_stepdown_nonce == 1U);
    TEST_ASSERT(node->cluster.cluster_id == 0U);
    TEST_ASSERT(node->cluster.term == 0U);
    TEST_ASSERT(node->cluster.head_node_id == 0U);
    TEST_ASSERT(node->cluster.head_lease_expires_at_ms == 0U);
    TEST_ASSERT(node->cluster.head_grace_deadline_ms == 0U);
    TEST_ASSERT(node->cluster.member_voted_term == 0U);
    TEST_ASSERT(node->cluster.known_backup_node_id == 0U);
    TEST_ASSERT(node->cluster.observation_deadline_ms != 0U);

    /* (b) A MEMBER already in takeover grace (armed deadline) detaches
     * from MEMBER_TAKEOVER_GRACE -> DETACHED_OBSERVE the same way (old
     * phase == GRACE). */
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    node->cluster.head_grace_deadline_ms = ucn_deadline_from_now(
        network.now_ms, node->cluster.config.keepalive_interval_ms);
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE;
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
    message.nonce = 2U; /* fresh: strictly greater than the fence */
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, network.nodes[0].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(node->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(node->cluster.transition_reason ==
                UCN_CLUSTER_REASON_STEPDOWN_ORDERED);
    TEST_ASSERT(node->cluster.last_stepdown_nonce == 2U);
    TEST_ASSERT(node->cluster.head_grace_deadline_ms == 0U);

    /* (c) CLV2-01-04c.5 (b.6 lesson): a MEMBER whose shadow is
     * deliberately desynced from legacy (shadow=DETACHED_OBSERVE while
     * role=MEMBER derives MEMBER_ACTIVE) makes the transition's
     * shadow==old_phase check fail (the FIRST validation - the derive
     * assert is never reached).  The transition is rejected fail-closed
     * and the anti-replay fence is NOT consumed: no half-commit. */
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    node->cluster.cluster_id = 1U;
    node->cluster.term = 5U;
    node->cluster.head_node_id = network.nodes[0].node_id;
    node->cluster.last_stepdown_nonce = 2U;
    ucn_cluster_test_transition_asserts_set(false); /* survive the knob assert */
    {
        uint32_t before_nonce = node->cluster.last_stepdown_nonce;
        uint32_t before_cid = node->cluster.cluster_id;
        uint32_t before_term = node->cluster.term;
        ucn_node_id_t before_head = node->cluster.head_node_id;

        (void)memset(&message, 0, sizeof(message));
        message.type = UCN_CLUSTER_MSG_HEAD_STEPDOWN;
        message.role = UCN_CLUSTER_ROLE_HEAD;
        message.cluster_id = 1U;
        message.term = 5U;
        message.head_node_id = network.nodes[0].node_id;
        message.lease_ms = 8000U;
        message.nonce = 3U; /* fresh: strictly greater than the fence */
        TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        TEST_ASSERT(ucn_cluster_receive(&node->cluster,
                                        network.nodes[0].node_id,
                                        true, encoded, sizeof(encoded)) ==
                    UCN_ERR_STATE);
        /* THE b.6 fix: the fence is NOT consumed by a rejected transition. */
        TEST_ASSERT(node->cluster.last_stepdown_nonce == before_nonce);
        /* No partial commit: the legacy detach never ran. */
        TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
        TEST_ASSERT(node->cluster.cluster_id == before_cid);
        TEST_ASSERT(node->cluster.term == before_term);
        TEST_ASSERT(node->cluster.head_node_id == before_head);
        /* The phase did NOT migrate: the end-of-RX shadow sync re-aligns
         * the mirror to the UNCHANGED legacy (derive==MEMBER_ACTIVE), so
         * shadow returns to MEMBER_ACTIVE - it never advanced to
         * DETACHED_OBSERVE and no STEPDOWN_ORDERED was recorded. */
        TEST_ASSERT(node->cluster.shadow_phase ==
                    UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
        TEST_ASSERT(node->cluster.transition_reason !=
                    UCN_CLUSTER_REASON_STEPDOWN_ORDERED);
    }
    ucn_cluster_test_transition_asserts_set(true);
    return 0;
}

/* CLV2-01-04e.7 (human audit MAJOR 2.C): the BACKUP sub-branch of the
 * HEAD_STEPDOWN handler now routes through the single transition entry
 * point with the b.6 fail-closed discipline: the old phase is derived
 * from the PRE-CALL state (takeover_active -> BACKUP_TAKEOVER, ready ->
 * BACKUP_READY, else BACKUP_SYNCING), the transition runs FIRST with the
 * EXPLICIT STEPDOWN_ORDERED reason (an ordered stepdown is an ordered
 * stepdown regardless of role - never the PRIMARY_LOST / TAKEOVER_TIMEOUT
 * BEST-EFFORT fallbacks), and the anti-replay fence is consumed +
 * backup_clear_sync() runs ONLY on success.  A late Type12 during
 * takeover (M01.0.2 combo) must never be rejected for phase reasons. */
static int cluster_test_backup_stepdown_shadow_transition(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_t *c;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 0U;
    node = &network.nodes[2];
    c = &node->cluster;

    /* (a) A BACKUP_READY node receives a HEAD_STEPDOWN of the CURRENT
     * epoch with a fresh nonce: BACKUP_READY -> DETACHED_OBSERVE through
     * the entry point with the EXPLICIT STEPDOWN_ORDERED reason (never
     * the PRIMARY_LOST BEST-EFFORT fallback), the fence advances, and
     * backup_clear_sync() still runs in its original order (nonce first,
     * then the mirror clear). */
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;
    c->transition_reason = UCN_CLUSTER_REASON_INIT;
    c->shadow_transition_count = 0U;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = network.nodes[0].node_id;
    c->backup_primary_node_id = network.nodes[0].node_id;
    c->backup_generation = 7U;
    c->backup_ready = true;
    c->backup_syncing = false;
    c->backup_takeover_active = false;
    c->membership_sequence = 42U;
    c->backup_primary_deadline_ms = 8000U;
    c->backup_missed_heartbeats = 3U;
    c->known_backup_node_id = network.nodes[0].node_id;
    c->known_backup_generation = 7U;
    c->recovery_eligible = false;
    c->last_stepdown_nonce = 0U;
    (void)memset(c->primary_members.slots, 0, sizeof(c->primary_members.slots));
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = 9U;
    TEST_ASSERT(test_legacy_state_valid(c) == true);
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
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
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_STEPDOWN_ORDERED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    /* fence advanced; the site-side backup_clear_sync() ran in its
     * original order (nonce first, then the mirror clear) */
    TEST_ASSERT(c->last_stepdown_nonce == 1U);
    TEST_ASSERT(c->backup_syncing == false);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->backup_primary_node_id == 0U);
    TEST_ASSERT(c->backup_generation == 0U);
    TEST_ASSERT(c->membership_sequence == 0U);
    TEST_ASSERT(c->backup_primary_deadline_ms == 0U);
    TEST_ASSERT(c->backup_missed_heartbeats == 0U);
    TEST_ASSERT(c->primary_members.slots[0].occupied == false);
    TEST_ASSERT(c->known_backup_node_id == 0U);
    TEST_ASSERT(c->known_backup_generation == 0U);
    TEST_ASSERT(c->cluster_id == 0U);
    TEST_ASSERT(c->term == 0U);
    TEST_ASSERT(c->head_node_id == 0U);
    TEST_ASSERT(c->head_grace_deadline_ms == 0U);
    TEST_ASSERT(c->observation_deadline_ms != 0U);

    /* (b) A BACKUP_SYNCING node (ready=false, syncing=true) detaches the
     * same way: BACKUP_SYNCING -> DETACHED_OBSERVE, STEPDOWN_ORDERED. */
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_SYNCING;
    c->transition_reason = UCN_CLUSTER_REASON_INIT;
    c->shadow_transition_count = 0U;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = network.nodes[0].node_id;
    c->backup_primary_node_id = network.nodes[0].node_id;
    c->backup_generation = 7U;
    c->backup_ready = false;
    c->backup_syncing = true;
    c->backup_takeover_active = false;
    c->membership_sequence = 42U;
    c->backup_primary_deadline_ms = 8000U;
    c->backup_missed_heartbeats = 3U;
    c->known_backup_node_id = network.nodes[0].node_id;
    c->known_backup_generation = 7U;
    c->recovery_eligible = false;
    c->last_stepdown_nonce = 1U;
    TEST_ASSERT(test_legacy_state_valid(c) == true);
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_STEPDOWN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 5U;
    message.head_node_id = network.nodes[0].node_id;
    message.lease_ms = 8000U;
    message.nonce = 2U; /* fresh: strictly greater than the fence */
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, network.nodes[0].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_STEPDOWN_ORDERED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->last_stepdown_nonce == 2U);
    TEST_ASSERT(c->backup_syncing == false);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->backup_primary_node_id == 0U);
    TEST_ASSERT(c->backup_generation == 0U);
    TEST_ASSERT(c->membership_sequence == 0U);
    TEST_ASSERT(c->backup_primary_deadline_ms == 0U);
    TEST_ASSERT(c->backup_missed_heartbeats == 0U);

    /* (c) M01.0.2: a BACKUP_TAKEOVER node still holding the
     * takeover_active && backup_syncing combo detaches through the same
     * DIRECT edge (BACKUP_TAKEOVER -> DETACHED_OBSERVE exists for the
     * stepdown path).  A late Type12 during takeover must never be
     * rejected for phase reasons - the combo is REACHABLE and must stay
     * expressible. */
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
    c->transition_reason = UCN_CLUSTER_REASON_INIT;
    c->shadow_transition_count = 0U;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = network.nodes[0].node_id;
    c->backup_primary_node_id = network.nodes[0].node_id;
    c->backup_generation = 7U;
    c->backup_ready = false;
    c->backup_syncing = true; /* M01.0.2 combo */
    c->backup_takeover_active = true;
    c->membership_sequence = 42U;
    c->backup_primary_deadline_ms = 8000U;
    c->backup_missed_heartbeats = 3U;
    c->known_backup_node_id = network.nodes[0].node_id;
    c->known_backup_generation = 7U;
    c->recovery_eligible = false;
    c->last_stepdown_nonce = 2U;
    TEST_ASSERT(test_legacy_state_valid(c) == true);
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_STEPDOWN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 5U;
    message.head_node_id = network.nodes[0].node_id;
    message.lease_ms = 8000U;
    message.nonce = 3U; /* fresh: strictly greater than the fence */
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, network.nodes[0].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_STEPDOWN_ORDERED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->last_stepdown_nonce == 3U);
    TEST_ASSERT(c->backup_syncing == false);
    TEST_ASSERT(c->backup_ready == false);
    /* Legacy backup_clear_sync() never cleared takeover_active (it is
     * role-BACKUP-only state; the node is DETACHED now and still derives
     * DETACHED_OBSERVE) - the migration must not add a clear the legacy
     * site did not perform. */
    TEST_ASSERT(c->backup_takeover_active == true);
    TEST_ASSERT(c->backup_primary_node_id == 0U);
    TEST_ASSERT(c->backup_generation == 0U);
    TEST_ASSERT(c->membership_sequence == 0U);
    TEST_ASSERT(c->backup_primary_deadline_ms == 0U);
    TEST_ASSERT(c->backup_missed_heartbeats == 0U);

    /* (d) CLV2-01-04e.7 (b.6 lesson): a BACKUP whose shadow is
     * deliberately desynced from legacy (shadow=DETACHED_OBSERVE while
     * role=BACKUP derives BACKUP_READY) makes the transition's
     * shadow==old_phase check fail (the FIRST validation - the derive
     * assert is never reached).  The transition is rejected fail-closed
     * and the anti-replay fence is NOT consumed: no half-commit. */
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE; /* stale */
    c->transition_reason = UCN_CLUSTER_REASON_INIT;
    c->shadow_transition_count = 0U;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = network.nodes[0].node_id;
    c->backup_primary_node_id = network.nodes[0].node_id;
    c->backup_generation = 7U;
    c->backup_ready = true;
    c->backup_syncing = false;
    c->backup_takeover_active = false;
    c->membership_sequence = 42U;
    c->backup_primary_deadline_ms = 8000U;
    c->backup_missed_heartbeats = 3U;
    c->known_backup_node_id = network.nodes[0].node_id;
    c->known_backup_generation = 7U;
    c->recovery_eligible = false;
    c->last_stepdown_nonce = 3U;
    (void)memset(c->primary_members.slots, 0, sizeof(c->primary_members.slots));
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = 9U;
    ucn_cluster_test_transition_asserts_set(false); /* survive the knob assert */
    {
        uint32_t before_nonce = c->last_stepdown_nonce;
        ucn_node_id_t before_primary = c->backup_primary_node_id;
        uint32_t before_generation = c->backup_generation;
        uint32_t before_sequence = c->membership_sequence;
        uint32_t before_deadline = c->backup_primary_deadline_ms;
        uint8_t before_missed = c->backup_missed_heartbeats;
        ucn_node_id_t before_known_node = c->known_backup_node_id;
        uint32_t before_known_gen = c->known_backup_generation;
        uint32_t before_cid = c->cluster_id;
        uint32_t before_term = c->term;
        ucn_node_id_t before_head = c->head_node_id;

        (void)memset(&message, 0, sizeof(message));
        message.type = UCN_CLUSTER_MSG_HEAD_STEPDOWN;
        message.role = UCN_CLUSTER_ROLE_HEAD;
        message.cluster_id = 1U;
        message.term = 5U;
        message.head_node_id = network.nodes[0].node_id;
        message.lease_ms = 8000U;
        message.nonce = 4U; /* fresh: strictly greater than the fence */
        TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        TEST_ASSERT(ucn_cluster_receive(&node->cluster,
                                        network.nodes[0].node_id,
                                        true, encoded, sizeof(encoded)) ==
                    UCN_ERR_STATE);
        /* THE b.6 fix: the fence is NOT consumed by a rejected transition. */
        TEST_ASSERT(c->last_stepdown_nonce == before_nonce);
        /* No partial commit: the legacy backup_clear_sync() never ran. */
        TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_BACKUP);
        TEST_ASSERT(c->backup_ready == true);
        TEST_ASSERT(c->backup_syncing == false);
        TEST_ASSERT(c->backup_takeover_active == false);
        TEST_ASSERT(c->backup_primary_node_id == before_primary);
        TEST_ASSERT(c->backup_generation == before_generation);
        TEST_ASSERT(c->membership_sequence == before_sequence);
        TEST_ASSERT(c->backup_primary_deadline_ms == before_deadline);
        TEST_ASSERT(c->backup_missed_heartbeats == before_missed);
        TEST_ASSERT(c->known_backup_node_id == before_known_node);
        TEST_ASSERT(c->known_backup_generation == before_known_gen);
        TEST_ASSERT(c->cluster_id == before_cid);
        TEST_ASSERT(c->term == before_term);
        TEST_ASSERT(c->head_node_id == before_head);
        TEST_ASSERT(c->primary_members.slots[0].occupied == true);
        /* The phase did NOT migrate: the end-of-RX shadow sync re-aligns
         * the mirror to the UNCHANGED legacy (derive==BACKUP_READY) - it
         * never advanced to DETACHED_OBSERVE through the entry point. */
        TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_READY);
    }
    ucn_cluster_test_transition_asserts_set(true);

    /* (e) A STALE nonce of the same epoch is fenced BEFORE any transition
     * is attempted (existing behaviour): no state change, no reason
     * change, and the fence itself is untouched. */
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;
    c->transition_reason = UCN_CLUSTER_REASON_INIT;
    c->shadow_transition_count = 0U;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = network.nodes[0].node_id;
    c->backup_primary_node_id = network.nodes[0].node_id;
    c->backup_generation = 7U;
    c->backup_ready = true;
    c->backup_syncing = false;
    c->backup_takeover_active = false;
    c->last_stepdown_nonce = 5U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_STEPDOWN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 5U;
    message.head_node_id = network.nodes[0].node_id;
    message.lease_ms = 8000U;
    message.nonce = 5U; /* not strictly greater than last_stepdown_nonce */
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, network.nodes[0].node_id,
                                    true, encoded, sizeof(encoded)) ==
                UCN_ERR_ACCESS);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_READY);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_INIT);
    TEST_ASSERT(c->last_stepdown_nonce == 5U);
    TEST_ASSERT(c->backup_ready == true);
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

    TEST_ASSERT(UCN_CLUSTER_PHASE_COUNT == 21);
    /* CLV2-M12 (12-07): +1 for UCN_CLUSTER_REASON_STABLE_RECLAIM. */
    TEST_ASSERT(UCN_CLUSTER_REASON_COUNT == 33);
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
    cluster_fixture_set_role(c, UCN_CLUSTER_ROLE_TERM_CONFLICT);
    TEST_ASSERT(test_derive_phase(c, now) ==
                UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT);

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

/* CLV2-01-04c: the member lease-expiry / grace-timeout path flows through
 * cluster_transition().  (a) lease expiry arms the grace: the FIRST GRACE
 * tick must pin reason HEAD_LEASE_EXPIRED with an armed grace deadline
 * (role stays MEMBER); (b) the grace deadline then expires with no Head:
 * RECOVERY_OBSERVE pinned to reason GRACE_TIMEOUT (role DETACHED,
 * recovery_eligible, grace disarmed, no backoff).  The shadow audit
 * already covers the phase sequence; these asserts pin the reasons at the
 * right ticks. */
static int cluster_test_lease_grace_reasons(void)
{
    cluster_test_network_t network;
    ucn_cluster_t *member;
    uint32_t now_ms;
    bool saw_grace_arm = false;
    bool saw_grace_timeout = false;

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
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
        /* (a) lease expiry arms the grace: the FIRST GRACE tick carries
         * reason HEAD_LEASE_EXPIRED, role stays MEMBER and the deadline
         * is armed (shadow == derived == MEMBER_TAKEOVER_GRACE). */
        if (!saw_grace_arm &&
            member->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE) {
            TEST_ASSERT(member->transition_reason ==
                        UCN_CLUSTER_REASON_HEAD_LEASE_EXPIRED);
            TEST_ASSERT(member->role == UCN_CLUSTER_ROLE_MEMBER);
            TEST_ASSERT(member->head_grace_deadline_ms != 0U);
            TEST_ASSERT(member->stats.head_leases_expired == 1U);
            saw_grace_arm = true;
        }
        /* (b) the grace deadline expires with no Head: RECOVERY_OBSERVE
         * carries reason GRACE_TIMEOUT with role DETACHED, eligibility
         * armed and the grace deadline disarmed. */
        if (!saw_grace_timeout &&
            member->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE) {
            TEST_ASSERT(member->transition_reason ==
                        UCN_CLUSTER_REASON_GRACE_TIMEOUT);
            TEST_ASSERT(member->role == UCN_CLUSTER_ROLE_DETACHED);
            TEST_ASSERT(member->recovery_eligible == true);
            TEST_ASSERT(member->head_grace_deadline_ms == 0U);
            saw_grace_timeout = true;
        }
    }
    TEST_ASSERT(saw_grace_arm);
    TEST_ASSERT(saw_grace_timeout);
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
            message.membership_sequence = 1U;
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
    config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
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

/* CLV2-M04 (04-03/04-04): configuration is fail-closed by default and a
 * REQUIRED Provider loads before Cluster state exists.  The test deliberately
 * stops before any promise-bearing FSM transition. */
static int cluster_test_persistence_init_restore(void)
{
    cluster_test_network_t network;
    cluster_test_node_t node;
    cluster_test_persistence_probe_t probe;
    ucn_cluster_config_t config;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_view_t view;
    const ucn_cluster_stats_t *stats;

    (void)memset(&network, 0, sizeof(network));
    (void)memset(&node, 0, sizeof(node));
    (void)memset(&probe, 0, sizeof(probe));
    cluster_test_persist_set_factory(&probe);
    node.network = &network;
    node.node_id = 1U;
    node.alive = true;

    (void)memset(&config, 0, sizeof(config));
    config.local_node_id = node.node_id;
    config.enabled = true;
    config.head_capable = false;
    config.now_ms = cluster_test_now;
    config.now_context = &node;
    config.send = cluster_test_send;
    config.send_context = &node;

    /* The zero/default mode is REQUIRED, so legacy no-provider setup cannot
     * silently operate as a reboot-safe Cluster. */
    TEST_ASSERT(ucn_cluster_init(&node.cluster, &config) == UCN_ERR_CONFIG);
    config.persistence_mode = (ucn_cluster_persistence_mode_t)99;
    TEST_ASSERT(ucn_cluster_init(&node.cluster, &config) == UCN_ERR_CONFIG);

    config.persistence_mode = UCN_CLUSTER_PERSISTENCE_REQUIRED;
    (void)memset(&provider, 0, sizeof(provider));
    config.persistence_provider = &provider;
    TEST_ASSERT(ucn_cluster_init(&node.cluster, &config) == UCN_ERR_CONFIG);

    provider.struct_size = (uint16_t)sizeof(provider);
    provider.api_version = UCN_CLUSTER_PERSIST_PROVIDER_API_VERSION;
    provider.load = cluster_test_persist_load;
    provider.submit = cluster_test_persist_submit;
    provider.context = &probe;
    TEST_ASSERT(ucn_cluster_init(&node.cluster, &config) == UCN_OK);
    TEST_ASSERT(probe.load_calls == 3U && probe.submit_calls == 1U);
    TEST_ASSERT(network.queue_count == 0U);
    TEST_ASSERT(ucn_cluster_get_view(&node.cluster, &view) == UCN_OK);
    TEST_ASSERT(view.persistence_mode == UCN_CLUSTER_PERSISTENCE_REQUIRED);
    TEST_ASSERT(view.persistence_restore_state ==
                UCN_CLUSTER_PERSISTENCE_RESTORE_FACTORY_EMPTY);
    stats = ucn_cluster_get_stats(&node.cluster);
    TEST_ASSERT(stats != NULL &&
                stats->persistence_mode == UCN_CLUSTER_PERSISTENCE_REQUIRED &&
                stats->persistence_restore_state ==
                    UCN_CLUSTER_PERSISTENCE_RESTORE_FACTORY_EMPTY);

    /* A READY record restores only the existing Current-FSM safety inputs,
     * including its three-field vote replay key (not the full persisted
     * VoteId). Config/Rekey/Tombstone remain validated Provider authority
     * until their later owners attach, so this init cannot create an outward
     * promise. */
    (void)memset(&probe.loaded, 0, sizeof(probe.loaded));
    probe.load_result = UCN_OK;
    probe.loaded.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    ucn_cluster_persist_state_init_empty(&probe.loaded.snapshot);
    probe.loaded.snapshot.has_active_epoch = true;
    probe.loaded.snapshot.active_epoch.cluster_id = 7U;
    probe.loaded.snapshot.active_epoch.term = 9U;
    probe.loaded.snapshot.active_epoch.head_node_id = 5U;
    probe.loaded.snapshot.has_max_epoch = true;
    probe.loaded.snapshot.max_epoch = probe.loaded.snapshot.active_epoch;
    probe.loaded.snapshot.last_vote.valid = true;
    probe.loaded.snapshot.last_vote.epoch = probe.loaded.snapshot.active_epoch;
    probe.loaded.snapshot.last_vote.voted_for_node_id = 6U;
    probe.loaded.snapshot.last_vote.backup_generation = 2U;
    probe.loaded.snapshot.boot_incarnation = 11U;
    config.cluster_id_incarnation = 3U;
    TEST_ASSERT(ucn_cluster_init(&node.cluster, &config) == UCN_OK);
    TEST_ASSERT(probe.load_calls == 6U && probe.submit_calls == 2U &&
                network.queue_count == 0U);
    TEST_ASSERT(node.cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(node.cluster.cluster_id == 0U && node.cluster.term == 0U);
    TEST_ASSERT(node.cluster.last_cluster_id == 7U &&
                node.cluster.max_seen_term == 9U &&
                node.cluster.last_stable_head == 5U);
    TEST_ASSERT(node.cluster.member_voted_cluster_id == 7U &&
                node.cluster.member_voted_term == 9U &&
                node.cluster.member_voted_generation == 2U);
    TEST_ASSERT(node.cluster.config.cluster_id_incarnation == 12U);
    TEST_ASSERT(ucn_cluster_get_view(&node.cluster, &view) == UCN_OK &&
                view.persistence_restore_state ==
                    UCN_CLUSTER_PERSISTENCE_RESTORE_READY);

    /* A transport/CRC failure and an impossible success result both leave no
     * partially initialized Cluster object behind. */
    probe.load_result = UCN_ERR_CRC;
    (void)memset(&node.cluster, 0xA5, sizeof(node.cluster));
    TEST_ASSERT(ucn_cluster_init(&node.cluster, &config) == UCN_ERR_CRC);
    TEST_ASSERT(cluster_test_bytes_are_zero(&node.cluster,
                                            sizeof(node.cluster)));
    probe.load_result = UCN_OK;
    (void)memset(&probe.loaded, 0, sizeof(probe.loaded));
    (void)memset(&node.cluster, 0xA5, sizeof(node.cluster));
    TEST_ASSERT(ucn_cluster_init(&node.cluster, &config) == UCN_ERR_CONFIG);
    TEST_ASSERT(cluster_test_bytes_are_zero(&node.cluster,
                                            sizeof(node.cluster)));

    config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
    config.persistence_provider = NULL;
    TEST_ASSERT(ucn_cluster_init(&node.cluster, &config) == UCN_OK);
    TEST_ASSERT(probe.load_calls == 8U);
    TEST_ASSERT(ucn_cluster_get_view(&node.cluster, &view) == UCN_OK);
    TEST_ASSERT(view.persistence_mode ==
                UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST &&
                view.persistence_restore_state ==
                    UCN_CLUSTER_PERSISTENCE_RESTORE_NOT_APPLICABLE);
    stats = ucn_cluster_get_stats(&node.cluster);
    TEST_ASSERT(stats != NULL &&
                stats->persistence_mode ==
                    UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST &&
                stats->persistence_restore_state ==
                    UCN_CLUSTER_PERSISTENCE_RESTORE_NOT_APPLICABLE);
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
    input.role = UCN_CLUSTER_ROLE_MEMBER;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) ==
                UCN_ERR_ARGUMENT);
    input.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    input.cluster_id = UCN_NODE_BROADCAST;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) ==
                UCN_ERR_ARGUMENT);
    input.cluster_id = 13U;
    input.recovery_parent_cluster_id = UCN_NODE_BROADCAST;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) ==
                UCN_ERR_ARGUMENT);
    input.recovery_parent_cluster_id = 13U;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) ==
                UCN_ERR_ARGUMENT);

    /* Type 17 RECOVERY_ACK: exact non-zero round + lineage echo. */
    (void)memset(&input, 0, sizeof(input));
    input.type = UCN_CLUSTER_MSG_RECOVERY_ACK;
    input.role = UCN_CLUSTER_ROLE_MEMBER;
    input.cluster_id = 13U;
    input.term = 2U;
    input.head_node_id = 4U;
    input.recovery_nonce = 12345U;
    input.recovery_parent_cluster_id = 9U;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_OK);
    TEST_ASSERT(output.type == UCN_CLUSTER_MSG_RECOVERY_ACK &&
                output.recovery_nonce == 12345U &&
                output.recovery_parent_cluster_id == 9U);
    input.recovery_nonce = 0U;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) ==
                UCN_ERR_ARGUMENT);
    input.recovery_nonce = 12345U;
    input.role = UCN_CLUSTER_ROLE_HEAD;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) ==
                UCN_ERR_ARGUMENT);
    input.role = UCN_CLUSTER_ROLE_MEMBER;
    input.cluster_id = UCN_NODE_BROADCAST;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) ==
                UCN_ERR_ARGUMENT);
    input.cluster_id = 13U;
    input.recovery_parent_cluster_id = UCN_NODE_BROADCAST;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) ==
                UCN_ERR_ARGUMENT);
    input.recovery_parent_cluster_id = 13U;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) ==
                UCN_ERR_ARGUMENT);

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
    /* CLV2-01-04d.3: the manual staging must align the shadow mirror
     * with the derived HEAD_BACKUP_SYNCING phase (selected Backup, sweep
     * complete, snapshot in flight) - the migrated READY handler
     * validates shadow == old before the STABLE transition. */
    head->cluster.backup_assign_pending = false;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
    /* CLV2-01-04e.1: the manual staging must align the shadow mirror
     * with the derived MEMBER_ACTIVE phase - the migrated BACKUP_ASSIGN
     * handler validates shadow == old before the BACKUP_SYNCING
     * transition (shadow-guard closure). */
    backup->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    backup->cluster.cluster_id = 1U;
    backup->cluster.term = 1U;
    backup->cluster.head_node_id = head->node_id;
    backup->cluster.config.head_capable = true;
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
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


/* CLV2-01-04e.2: handle_backup_member_sync() SYNC_END wiring through
 * the single transition entry point.  The SYNC_END-with-full-coverage
 * event decides the BACKUP_SYNCING -> BACKUP_READY transition
 * (SNAPSHOT_READY); cluster_transition() validates whether the shadow
 * agrees and fails closed (UCN_ERR_STATE, zero writes) on a mismatch -
 * the Backup is never marked ready against a desynced shadow.  The
 * transition runs BEFORE the site's syncing=false/ready=true writes;
 * SYNC_BEGIN / DELTA-gap / member-data paths stay legacy (no
 * transition).  M01.0.2: a late SYNC_END while takeover is active keeps
 * the phase BACKUP_TAKEOVER (takeover precedence) - no transition, the
 * legacy body still applies the sync frames. */
static int cluster_test_backup_sync_transition(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    cluster_test_node_t *backup;
    cluster_test_node_t *third;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    size_t count_takeover;
    size_t count_prev;
    ucn_result_t result;

    /* Stage head=HEAD, backup + third = MEMBER over the fully-connected
     * test network; a BACKUP_ASSIGN turns the designated node into a
     * syncing Backup (the post-RX mirror sync aligns shadow with the
     * derived BACKUP_SYNCING). */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];
    backup = &network.nodes[1];
    third = &network.nodes[2];
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = backup->node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.backup_ready = false;
    head->cluster.backup_assign_pending = false;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
    backup->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    backup->cluster.cluster_id = 1U;
    backup->cluster.term = 1U;
    backup->cluster.head_node_id = head->node_id;
    backup->cluster.config.head_capable = true;
    /* CLV2-01-04e.1 (e-group merge cross-point): the manual staging must
     * align the shadow mirror with the derived MEMBER_ACTIVE phase - the
     * migrated BACKUP_ASSIGN handler validates shadow == old before the
     * BACKUP_SYNCING transition (shadow-guard closure), exactly like the
     * sibling staging at cluster_test_backup_assign_transition(). */
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    third->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    third->cluster.cluster_id = 1U;
    third->cluster.term = 1U;
    third->cluster.head_node_id = head->node_id;
    third->cluster.config.head_capable = false;
    network.now_ms = 0U;
    TEST_ASSERT(cluster_test_sync_neighbors(&network) == 0);

    /* BACKUP_ASSIGN -> role BACKUP, syncing, shadow aligned. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_ASSIGN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.sync_token = backup->node_id;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(backup->cluster.backup_syncing == true);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    /* Reset the shadow bookkeeping so every transition-count assertion
     * below measures THIS scenario only. */
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_SYNCING;
    backup->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    backup->cluster.shadow_transition_count = 0U;

    /* (b) SYNC_BEGIN re-enters SYNCING with NO transition: count and
     * shadow stay put. */
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
    TEST_ASSERT(backup->cluster.backup_syncing == true);
    TEST_ASSERT(backup->cluster.backup_ready == false);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(backup->cluster.shadow_transition_count == 0U);

    /* (c) DELTA gap: stays SYNCING, RESYNC_REQ queued, NO transition. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.flags = UCN_CLUSTER_FLAG_SYNC_DELTA;
    message.backup_generation = 1U;
    message.membership_sequence = 5U; /* gap: expected 2 */
    message.member_node_id = third->node_id;
    message.member_lease_ms = 8000U;
    message.member_nonce = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(backup->cluster.backup_syncing == true);
    TEST_ASSERT(backup->cluster.backup_ready == false);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(backup->cluster.shadow_transition_count == 0U);
    TEST_ASSERT(network.queue_count == 1U); /* BACKUP_RESYNC_REQ pending. */
    network.queue_count = 0U;

    /* Member record (seq 2): the mirror now covers node 2 (fully
     * connected admitted peers) so the END has full coverage. */
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
    TEST_ASSERT(backup->cluster.shadow_transition_count == 0U);

    /* (a) SYNC_END with full coverage: shadow == BACKUP_READY with
     * reason SNAPSHOT_READY, count +1, ready=true, BACKUP_READY sent. */
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
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    TEST_ASSERT(backup->cluster.transition_reason ==
                UCN_CLUSTER_REASON_SNAPSHOT_READY);
    TEST_ASSERT(backup->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(backup->cluster.backup_ready == true);
    TEST_ASSERT(backup->cluster.backup_syncing == false);
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    TEST_ASSERT(network.queue_count == 1U); /* BACKUP_READY pending. */
    network.queue_count = 0U;

    /* CLV2-01-04e.7: re-enter SYNCING with a fresh snapshot BEGIN for the
     * desync scenario (a fresh BEGIN restarts the membership_sequence).
     * The BEGIN from BACKUP_READY now commits an EXPLICIT READY -> SYNCING
     * transition (RESYNC_STARTED) BEFORE the site's mirror/sequence/
     * syncing=true/ready=false writes - the old hand-reset that masked
     * the implicit end-of-RX mint is gone. */
    count_prev = backup->cluster.shadow_transition_count; /* == 1 (the (a) END) */
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
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(backup->cluster.transition_reason ==
                UCN_CLUSTER_REASON_RESYNC_STARTED);
    TEST_ASSERT(backup->cluster.shadow_transition_count ==
                count_prev + 1U);
    TEST_ASSERT(backup->cluster.backup_syncing == true);
    TEST_ASSERT(backup->cluster.backup_ready == false);
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
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

    /* (d) shadow-desync at SYNC_END: the legacy event still decides the
     * transition (derive == BACKUP_SYNCING), so the transition is called
     * UNCONDITIONALLY; the shadow validate gate rejects (claims
     * BACKUP_READY) and the site fails closed - ready is NOT set.  The
     * rejection asserts are silenced (framework convention) so the
     * release UCN_ERR_STATE path can be verified. */
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY; /* desync */
    ucn_cluster_test_transition_asserts_set(false);
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
    result = ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                 encoded, sizeof(encoded));
    TEST_ASSERT(result == UCN_ERR_STATE);
    TEST_ASSERT(backup->cluster.backup_ready == false);  /* NOT set */
    TEST_ASSERT(backup->cluster.backup_syncing == true); /* NOT cleared */
    TEST_ASSERT(backup->cluster.shadow_phase !=
                UCN_CLUSTER_PHASE_BACKUP_READY);
    TEST_ASSERT(backup->cluster.transition_reason !=
                UCN_CLUSTER_REASON_SNAPSHOT_READY);
    /* The transition never committed: the end-of-RX mirror sync re-aligns
     * the shadow to the UNCHANGED legacy state (BACKUP_SYNCING), exactly
     * the CLV2-01-02 behaviour for a rejected message. */
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(network.queue_count == 0U); /* no BACKUP_READY sent */
    ucn_cluster_test_transition_asserts_set(true);

    /* (e) M01.0.2: a late SYNC_END during takeover.  First complete the
     * snapshot (SYNCING -> READY), then start the takeover the way the
     * real FSM does (READY -> BACKUP_TAKEOVER, TAKEOVER_STARTED, a DIRECT
     * edge - the lease-expiry trigger only fires on a READY Backup).  A
     * delayed old-Primary SYNC_BEGIN then re-arms syncing while takeover
     * stays active (the reachable M01.0.2 combo, exactly as
     * cluster_test_shadow_takeover_late_sync injects); the late END with
     * full coverage must NOT transition (derive == BACKUP_TAKEOVER takes
     * precedence) - the legacy body still applies the sync frames
     * (syncing=false, ready=true, BACKUP_READY sent) and the phase stays
     * BACKUP_TAKEOVER with takeover_active preserved. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.flags = UCN_CLUSTER_FLAG_SYNC_END;
    message.backup_generation = 1U;
    /* The rejected (d) END already advanced membership_sequence to 3
     * (the legacy sequence write precedes the SYNC_END branch), so the
     * re-completion END must carry sequence 4. */
    message.membership_sequence = 4U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    TEST_ASSERT(ucn_cluster_test_transition(
                    &backup->cluster, UCN_CLUSTER_PHASE_BACKUP_READY,
                    UCN_CLUSTER_PHASE_BACKUP_TAKEOVER,
                    UCN_CLUSTER_REASON_TAKEOVER_STARTED,
                    network.now_ms) == UCN_OK);
    TEST_ASSERT(backup->cluster.backup_takeover_active == true);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    network.queue_count = 0U; /* drop the (e)-start READY; only the late
                                 END's READY must remain pending */
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
    TEST_ASSERT(backup->cluster.backup_takeover_active == true);
    TEST_ASSERT(backup->cluster.backup_syncing == true);
    TEST_ASSERT(backup->cluster.backup_ready == false);
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
    count_takeover = backup->cluster.shadow_transition_count;
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
    TEST_ASSERT(backup->cluster.backup_takeover_active == true);
    TEST_ASSERT(backup->cluster.backup_ready == true);
    TEST_ASSERT(backup->cluster.backup_syncing == false);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    TEST_ASSERT(backup->cluster.shadow_transition_count == count_takeover);
    TEST_ASSERT(network.queue_count == 1U); /* BACKUP_READY pending. */
    return 0;
}

/* CLV2-01-04e.7: the member_sync RE-ENTRY edges that were still minted by
 * the end-of-RX shadow sync (the d.7.1-forbidden pattern) now commit
 * explicitly: a fresh SYNC_BEGIN / DELTA gap from BACKUP_READY is a
 * READY -> SYNCING transition (RESYNC_STARTED) run BEFORE the site's
 * syncing=true/ready=false writes, UNCONDITIONAL on the legacy event (the
 * derived pre-phase decides: BACKUP_READY -> transition, BACKUP_SYNCING ->
 * self no-op, BACKUP_TAKEOVER (M01.0.2) -> no transition, takeover
 * precedence).  Sub-cases:
 * (a) READY + shadow-desync + fresh SYNC_BEGIN -> fail closed
 *     (UCN_ERR_STATE, ready/syncing untouched, zero re-entry writes, the
 *     end-of-RX sync re-aligns to the unchanged READY - the only count
 *     bump is that re-align mint, never a committed RESYNC_STARTED);
 * (b) READY + DELTA gap -> explicit READY -> SYNCING + BACKUP_RESYNC_REQ
 *     still queued, legacy UCN_ERR_REPLAY preserved. */
static int cluster_test_backup_member_sync_resync_edges(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    cluster_test_node_t *backup;
    cluster_test_node_t *third;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_result_t result;
    size_t count_prev;

    /* Stage head=HEAD, backup + third = MEMBER over the fully-connected
     * test network; a BACKUP_ASSIGN turns the designated node into a
     * syncing Backup, a member record covers node 2, and the SYNC_END
     * completes the snapshot so the Backup reaches READY. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];
    backup = &network.nodes[1];
    third = &network.nodes[2];
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = backup->node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.backup_ready = false;
    head->cluster.backup_assign_pending = false;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
    backup->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    backup->cluster.cluster_id = 1U;
    backup->cluster.term = 1U;
    backup->cluster.head_node_id = head->node_id;
    backup->cluster.config.head_capable = true;
    /* The manual staging must align the shadow mirror with the derived
     * MEMBER_ACTIVE phase (e.1 cross-point), exactly like the sibling
     * staging in cluster_test_backup_sync_transition(). */
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    third->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    third->cluster.cluster_id = 1U;
    third->cluster.term = 1U;
    third->cluster.head_node_id = head->node_id;
    third->cluster.config.head_capable = false;
    network.now_ms = 0U;
    TEST_ASSERT(cluster_test_sync_neighbors(&network) == 0);

    /* BACKUP_ASSIGN -> role BACKUP, syncing, shadow aligned. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_ASSIGN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.sync_token = backup->node_id;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(backup->cluster.backup_syncing == true);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_SYNCING;
    backup->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    backup->cluster.shadow_transition_count = 0U;

    /* Member record (seq 1, right after the ASSIGN which reset the
     * sequence to 0): the mirror now covers node 2 so the later END has
     * full coverage. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.membership_sequence = 1U;
    message.member_node_id = third->node_id;
    message.member_lease_ms = 8000U;
    message.member_nonce = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);

    /* SYNC_END with full coverage -> READY (count == 1). */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.flags = UCN_CLUSTER_FLAG_SYNC_END;
    message.backup_generation = 1U;
    message.membership_sequence = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    TEST_ASSERT(backup->cluster.transition_reason ==
                UCN_CLUSTER_REASON_SNAPSHOT_READY);
    TEST_ASSERT(backup->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(backup->cluster.backup_ready == true);
    TEST_ASSERT(backup->cluster.backup_syncing == false);
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    TEST_ASSERT(network.queue_count == 1U); /* BACKUP_READY pending. */
    network.queue_count = 0U;

    /* (a) READY + shadow-desync + fresh SYNC_BEGIN -> fail closed.  The
     * legacy event still decides the transition (derive == BACKUP_READY),
     * so cluster_transition() is called UNCONDITIONALLY; the shadow
     * validate gate rejects (claims BACKUP_SYNCING) and the site fails
     * closed - ready/syncing are NOT touched and the re-entry writes
     * (mirror clear, sequence, syncing=true) never run.  The end-of-RX
     * mirror sync then re-aligns the shadow to the UNCHANGED derived
     * BACKUP_READY (the e.2(d) wrapper behaviour: the only count bump is
     * that re-align mint, never a committed RESYNC_STARTED). */
    count_prev = backup->cluster.shadow_transition_count; /* == 1 */
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_SYNCING; /* desync */
    ucn_cluster_test_transition_asserts_set(false);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.flags = UCN_CLUSTER_FLAG_SYNC_BEGIN;
    message.backup_generation = 1U;
    message.membership_sequence = 9U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    result = ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                 encoded, sizeof(encoded));
    TEST_ASSERT(result == UCN_ERR_STATE);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(backup->cluster.backup_ready == true);   /* NOT cleared */
    TEST_ASSERT(backup->cluster.backup_syncing == false); /* NOT set */
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_READY); /* re-aligned, NOT SYNCING */
    TEST_ASSERT(backup->cluster.transition_reason !=
                UCN_CLUSTER_REASON_RESYNC_STARTED);
    TEST_ASSERT(backup->cluster.shadow_transition_count == count_prev + 1U);
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    TEST_ASSERT(network.queue_count == 0U); /* nothing sent */

    /* (b) READY + DELTA gap -> explicit READY -> SYNCING (RESYNC_STARTED)
     * committed BEFORE the site's ready=false/syncing=true writes; the
     * legacy body still queues BACKUP_RESYNC_REQ and returns ERR_REPLAY. */
    count_prev = backup->cluster.shadow_transition_count; /* == 2 */
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.flags = UCN_CLUSTER_FLAG_SYNC_DELTA;
    message.backup_generation = 1U;
    message.membership_sequence = 8U; /* gap: expected 4 */
    message.member_node_id = third->node_id;
    message.member_lease_ms = 8000U;
    message.member_nonce = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(backup->cluster.transition_reason ==
                UCN_CLUSTER_REASON_RESYNC_STARTED);
    TEST_ASSERT(backup->cluster.shadow_transition_count == count_prev + 1U);
    TEST_ASSERT(backup->cluster.backup_syncing == true);
    TEST_ASSERT(backup->cluster.backup_ready == false);
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(network.queue_count == 1U); /* BACKUP_RESYNC_REQ pending. */
    network.queue_count = 0U;

    /* Re-complete the snapshot so (c)/(d)/(e) start from READY again:
     * the (b) DELTA gap left the node SYNCING (ready=false, syncing=true,
     * sequence unchanged at 2), so a full-coverage SYNC_END at seq 3
     * runs the SYNCING->READY transition (e.2, SNAPSHOT_READY). */
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
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    TEST_ASSERT(backup->cluster.transition_reason ==
                UCN_CLUSTER_REASON_SNAPSHOT_READY);
    TEST_ASSERT(backup->cluster.backup_ready == true);
    TEST_ASSERT(backup->cluster.backup_syncing == false);
    network.queue_count = 0U;

    /* (c) READY + shadow-desync + DELTA gap -> fail closed (review C
     * MINOR: the (b) happy path alone is mint-masked - the end-of-RX
     * sync reproduces RESYNC_STARTED + count+1, so removing the DELTA-gap
     * transition would fail no test; this sibling pins it).  The legacy
     * event still decides (derive == BACKUP_READY), the transition is
     * called UNCONDITIONALLY, the shadow validate gate rejects, and the
     * site fails closed - ready/syncing untouched, NO RESYNC_REQ queued,
     * the end-of-RX sync re-aligns the shadow to the unchanged READY. */
    count_prev = backup->cluster.shadow_transition_count;
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_SYNCING; /* desync */
    ucn_cluster_test_transition_asserts_set(false);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.flags = UCN_CLUSTER_FLAG_SYNC_DELTA;
    message.backup_generation = 1U;
    message.membership_sequence = 8U; /* gap: expected 4 */
    message.member_node_id = third->node_id;
    message.member_lease_ms = 8000U;
    message.member_nonce = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    result = ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                 encoded, sizeof(encoded));
    TEST_ASSERT(result == UCN_ERR_STATE);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(backup->cluster.backup_ready == true);   /* NOT cleared */
    TEST_ASSERT(backup->cluster.backup_syncing == false); /* NOT set */
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_READY); /* re-aligned, NOT SYNCING */
    TEST_ASSERT(backup->cluster.transition_reason !=
                UCN_CLUSTER_REASON_RESYNC_STARTED);
    TEST_ASSERT(backup->cluster.shadow_transition_count == count_prev + 1U);
    TEST_ASSERT(network.queue_count == 0U); /* NO RESYNC_REQ queued */
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_READY);

    /* (d) READY + plain snapshot sequence gap (no DELTA/BEGIN flag) ->
     * explicit READY -> SYNCING (RESYNC_STARTED), the third re-entry path
     * (review C MINOR: never exercised before).  The legacy body stays
     * syncing, resets the sequence and returns ERR_REPLAY. */
    count_prev = backup->cluster.shadow_transition_count; /* == 3 */
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.membership_sequence = 7U; /* gap: expected 5 */
    message.member_node_id = third->node_id; /* data frame requires it */
    message.member_nonce = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(backup->cluster.transition_reason ==
                UCN_CLUSTER_REASON_RESYNC_STARTED);
    TEST_ASSERT(backup->cluster.shadow_transition_count == count_prev + 1U);
    TEST_ASSERT(backup->cluster.backup_syncing == true);
    TEST_ASSERT(backup->cluster.backup_ready == false);
    TEST_ASSERT(backup->cluster.membership_sequence == 0U); /* reset */
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);

    /* Re-complete the snapshot so (e) starts from READY again: (d) reset
     * the sequence to 0 and left the node SYNCING, so re-run the member
     * record (seq 1) + full-coverage END (seq 2, SNAPSHOT_READY). */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.membership_sequence = 1U;
    message.member_node_id = third->node_id;
    message.member_lease_ms = 8000U;
    message.member_nonce = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.flags = UCN_CLUSTER_FLAG_SYNC_END;
    message.backup_generation = 1U;
    message.membership_sequence = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    TEST_ASSERT(backup->cluster.backup_ready == true);
    TEST_ASSERT(backup->cluster.backup_syncing == false);
    network.queue_count = 0U;

    /* (e) READY + shadow-desync + plain seq gap -> fail closed (the (d)
     * sibling discriminator: the end-of-RX mint alone would reproduce
     * RESYNC_STARTED + count+1, so this pins the explicit transition). */
    count_prev = backup->cluster.shadow_transition_count;
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_SYNCING; /* desync */
    ucn_cluster_test_transition_asserts_set(false);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.membership_sequence = 7U; /* gap: expected 4 */
    message.member_node_id = third->node_id; /* data frame requires it */
    message.member_nonce = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    result = ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                 encoded, sizeof(encoded));
    TEST_ASSERT(result == UCN_ERR_STATE);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(backup->cluster.backup_ready == true);   /* NOT cleared */
    TEST_ASSERT(backup->cluster.backup_syncing == false); /* NOT set */
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_READY); /* re-aligned, NOT SYNCING */
    TEST_ASSERT(backup->cluster.transition_reason !=
                UCN_CLUSTER_REASON_RESYNC_STARTED);
    TEST_ASSERT(backup->cluster.shadow_transition_count == count_prev + 1U);
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    return 0;
}

/* CLV2-01-04e.7: the member_sync DETACH paths no longer rely on the
 * end-of-RX mint: a coverage-failed SYNC_END and a mirror-allocation
 * failure now preflight + commit (SYNCING|READY|TAKEOVER) ->
 * DETACHED_OBSERVE with UCN_CLUSTER_REASON_PRIMARY_LOST BEFORE the
 * Current-order stats/send and the idempotent backup_clear_sync().
 * Reason choice (human auditor): for the SYNCING pre-state PRIMARY_LOST is
 * right (the primary's sync stream failed); for the TAKEOVER pre-state
 * TAKEOVER_TIMEOUT would be a lie (the takeover did NOT time out - a
 * sync-failure detach during takeover is reachable via the M01.0.2 late-
 * sync combo), so PRIMARY_LOST is used there too - the honest reason is
 * the primary's sync stream failure.  The TAKEOVER pre-state is NEVER
 * rejected on these paths just to avoid the edge: if the legacy body
 * detaches, the transition must express it.  Sub-cases:
 * (a) SYNCING + SYNC_END without coverage -> detach + reject sent +
 *     stats.joins_rejected++;
 * (b) SYNCING + desynced shadow + uncovered END -> fail closed: preflight
 *     rejects BEFORE any Current-order side effect (no stats++, no reject,
 *     no detach, mirror intact);
 * (c) SYNCING + mirror table full + member record -> detach + reject +
 *     UCN_ERR_NO_SPACE;
 * (d) TAKEOVER (M01.0.2 late-sync combo) + uncovered END -> the legacy
 *     body detaches and the transition must EXPRESS it (PRIMARY_LOST). */
static int cluster_test_stage_backup_syncing(
    cluster_test_network_t *network)
{
    cluster_test_node_t *head = &network->nodes[0];
    cluster_test_node_t *backup = &network->nodes[1];
    cluster_test_node_t *third = &network->nodes[2];
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = backup->node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.backup_ready = false;
    head->cluster.backup_assign_pending = false;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
    backup->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    backup->cluster.cluster_id = 1U;
    backup->cluster.term = 1U;
    backup->cluster.head_node_id = head->node_id;
    backup->cluster.config.head_capable = true;
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    third->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    third->cluster.cluster_id = 1U;
    third->cluster.term = 1U;
    third->cluster.head_node_id = head->node_id;
    third->cluster.config.head_capable = false;
    network->now_ms = 0U;
    TEST_ASSERT(cluster_test_sync_neighbors(network) == 0);

    /* BACKUP_ASSIGN -> role BACKUP, syncing, shadow aligned; reset the
     * shadow bookkeeping so every count assertion below measures THIS
     * scenario only. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_ASSIGN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.sync_token = backup->node_id;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(backup->cluster.backup_syncing == true);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_SYNCING;
    backup->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    backup->cluster.shadow_transition_count = 0U;

    /* Member record (seq 1, right after the ASSIGN which reset the
     * sequence to 0): the mirror covers node 2 (fully connected admitted
     * peers) so a later END would have full coverage. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.membership_sequence = 1U;
    message.member_node_id = third->node_id;
    message.member_lease_ms = 8000U;
    message.member_nonce = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.shadow_transition_count == 0U);
    return 0;
}

static int cluster_test_backup_member_sync_detach(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *backup;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_result_t result;
    size_t stats_prev;

    /* (a) SYNC_END without coverage from SYNCING -> explicit SYNCING ->
     * DETACHED_OBSERVE (PRIMARY_LOST), reject sent, stats.joins_rejected++.
     * Coverage fails because the mirror holds an extra member (node 5)
     * that has no admitted peer link. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    TEST_ASSERT(cluster_test_stage_backup_syncing(&network) == 0);
    backup = &network.nodes[1];
    backup->cluster.primary_members.slots[1].occupied = true; /* uncoverable */
    backup->cluster.primary_members.slots[1].node_id = 5U;
    stats_prev = backup->cluster.stats.joins_rejected;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = network.nodes[0].node_id;
    message.flags = UCN_CLUSTER_FLAG_SYNC_END;
    message.backup_generation = 1U;
    message.membership_sequence = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster,
                                    network.nodes[0].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(backup->cluster.transition_reason ==
                UCN_CLUSTER_REASON_PRIMARY_LOST);
    TEST_ASSERT(backup->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(backup->cluster.stats.joins_rejected == stats_prev + 1U);
    TEST_ASSERT(network.queue_count == 1U); /* BACKUP_REJECT pending. */
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(backup->cluster.recovery_eligible == false);
    TEST_ASSERT(backup->cluster.backup_syncing == false);
    TEST_ASSERT(backup->cluster.backup_ready == false);
    TEST_ASSERT(backup->cluster.backup_primary_node_id == 0U);
    TEST_ASSERT(backup->cluster.backup_generation == 0U);
    TEST_ASSERT(backup->cluster.membership_sequence == 0U);
    TEST_ASSERT(backup->cluster.backup_primary_deadline_ms == 0U);
    TEST_ASSERT(backup->cluster.primary_members.slots[0].occupied == false);
    TEST_ASSERT(backup->cluster.primary_members.slots[1].occupied == false);
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    network.queue_count = 0U;

    /* (b) SYNCING + desynced shadow + uncovered SYNC_END -> fail closed:
     * the preflight rejects BEFORE any Current-order side effect - no
     * stats++, no reject sent, no detach, mirror intact; the end-of-RX
     * sync re-aligns the shadow to the unchanged BACKUP_SYNCING (the only
     * count bump is that re-align mint, never a committed detach). */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    TEST_ASSERT(cluster_test_stage_backup_syncing(&network) == 0);
    backup = &network.nodes[1];
    backup->cluster.primary_members.slots[1].occupied = true; /* uncoverable */
    backup->cluster.primary_members.slots[1].node_id = 5U;
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE; /* desync */
    stats_prev = backup->cluster.stats.joins_rejected;
    ucn_cluster_test_transition_asserts_set(false);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = network.nodes[0].node_id;
    message.flags = UCN_CLUSTER_FLAG_SYNC_END;
    message.backup_generation = 1U;
    message.membership_sequence = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    result = ucn_cluster_receive(&backup->cluster,
                                 network.nodes[0].node_id, true,
                                 encoded, sizeof(encoded));
    TEST_ASSERT(result == UCN_ERR_STATE);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(backup->cluster.stats.joins_rejected == stats_prev); /* no ++ */
    TEST_ASSERT(network.queue_count == 0U); /* no reject sent */
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_BACKUP); /* NOT detached */
    TEST_ASSERT(backup->cluster.backup_syncing == true);
    TEST_ASSERT(backup->cluster.backup_ready == false);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING); /* re-aligned */
    TEST_ASSERT(backup->cluster.transition_reason !=
                UCN_CLUSTER_REASON_PRIMARY_LOST);
    TEST_ASSERT(backup->cluster.shadow_transition_count == 1U); /* re-align mint */
    TEST_ASSERT(backup->cluster.primary_members.slots[1].occupied == true); /* mirror intact */
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    network.queue_count = 0U;

    /* (c) mirror allocation failure (table full) -> detach + reject +
     * UCN_ERR_NO_SPACE.  All UCN_CLUSTER_MAX_MEMBERS slots are occupied by
     * foreign members, so the member record cannot allocate. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    TEST_ASSERT(cluster_test_stage_backup_syncing(&network) == 0);
    backup = &network.nodes[1];
    {
        size_t i;

        for (i = 0U; i < UCN_CLUSTER_MAX_MEMBERS; ++i) {
            backup->cluster.primary_members.slots[i].occupied = true;
            backup->cluster.primary_members.slots[i].node_id = (ucn_node_id_t)(10U + i);
        }
    }
    stats_prev = backup->cluster.stats.joins_rejected;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = network.nodes[0].node_id;
    message.backup_generation = 1U;
    message.membership_sequence = 2U; /* == seq 1 + 1 */
    message.member_node_id = 99U;     /* not present -> allocate fails */
    message.member_lease_ms = 8000U;
    message.member_nonce = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    result = ucn_cluster_receive(&backup->cluster,
                                 network.nodes[0].node_id, true,
                                 encoded, sizeof(encoded));
    TEST_ASSERT(result == UCN_ERR_NO_SPACE);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(backup->cluster.transition_reason ==
                UCN_CLUSTER_REASON_PRIMARY_LOST);
    TEST_ASSERT(backup->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(backup->cluster.stats.joins_rejected == stats_prev); /* no ++ */
    TEST_ASSERT(network.queue_count == 1U); /* BACKUP_REJECT pending. */
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(backup->cluster.backup_syncing == false);
    TEST_ASSERT(backup->cluster.backup_ready == false);
    TEST_ASSERT(backup->cluster.backup_primary_node_id == 0U);
    TEST_ASSERT(backup->cluster.membership_sequence == 0U);
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    network.queue_count = 0U;

    /* (c-desync) mirror-allocation failure + desynced shadow -> fail
     * closed (review C MINOR: the (c) happy path is mint-masked - the
     * end-of-RX sync reproduces PRIMARY_LOST + count+1, so removing the
     * alloc-fail preflight+commit would fail no test; this sibling pins
     * it).  The preflight rejects BEFORE any Current-order side effect:
     * no reject sent, no detach, mirror intact; the end-of-RX sync
     * re-aligns the shadow to the unchanged BACKUP_SYNCING. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    TEST_ASSERT(cluster_test_stage_backup_syncing(&network) == 0);
    backup = &network.nodes[1];
    {
        size_t i;

        for (i = 0U; i < UCN_CLUSTER_MAX_MEMBERS; ++i) {
            backup->cluster.primary_members.slots[i].occupied = true;
            backup->cluster.primary_members.slots[i].node_id = (ucn_node_id_t)(10U + i);
        }
    }
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE; /* desync */
    stats_prev = backup->cluster.stats.joins_rejected;
    ucn_cluster_test_transition_asserts_set(false);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = network.nodes[0].node_id;
    message.backup_generation = 1U;
    message.membership_sequence = 2U; /* == seq 1 + 1 */
    message.member_node_id = 99U;     /* not present -> allocate fails */
    message.member_lease_ms = 8000U;
    message.member_nonce = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    result = ucn_cluster_receive(&backup->cluster,
                                 network.nodes[0].node_id, true,
                                 encoded, sizeof(encoded));
    TEST_ASSERT(result == UCN_ERR_STATE);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(backup->cluster.stats.joins_rejected == stats_prev); /* no ++ */
    TEST_ASSERT(network.queue_count == 0U); /* no reject sent */
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_BACKUP); /* NOT detached */
    TEST_ASSERT(backup->cluster.backup_syncing == true);
    TEST_ASSERT(backup->cluster.backup_ready == false);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING); /* re-aligned */
    TEST_ASSERT(backup->cluster.transition_reason !=
                UCN_CLUSTER_REASON_PRIMARY_LOST);
    TEST_ASSERT(backup->cluster.shadow_transition_count == 1U); /* re-align mint */
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    network.queue_count = 0U;

    /* (d) M01.0.2: a takeover-active Backup (syncing re-armed by a delayed
     * Type12, the reachable late-sync combo) hitting the coverage-failed
     * END - the legacy body detaches, so the transition MUST express it:
     * the TAKEOVER pre-state is never rejected just to avoid the edge.
     * The reason is PRIMARY_LOST (the primary's sync stream failed), NOT
     * TAKEOVER_TIMEOUT - the takeover did not time out.  takeover_active
     * survives the detach exactly as the Current legacy body leaves it
     * (backup_clear_sync()/set_detached() never clear it; the e.5 timeout
     * path has the same shape). */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    TEST_ASSERT(cluster_test_stage_backup_syncing(&network) == 0);
    backup = &network.nodes[1];
    backup->cluster.backup_takeover_active = true; /* M01.0.2 combo */
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
    backup->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    backup->cluster.shadow_transition_count = 0U;
    TEST_ASSERT(test_legacy_state_valid(&backup->cluster) == true);
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    backup->cluster.primary_members.slots[1].occupied = true; /* uncoverable */
    backup->cluster.primary_members.slots[1].node_id = 5U;
    stats_prev = backup->cluster.stats.joins_rejected;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = network.nodes[0].node_id;
    message.flags = UCN_CLUSTER_FLAG_SYNC_END;
    message.backup_generation = 1U;
    message.membership_sequence = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster,
                                    network.nodes[0].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(backup->cluster.transition_reason ==
                UCN_CLUSTER_REASON_PRIMARY_LOST);
    TEST_ASSERT(backup->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(backup->cluster.stats.joins_rejected == stats_prev + 1U);
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(backup->cluster.backup_syncing == false);
    TEST_ASSERT(backup->cluster.backup_ready == false);
    TEST_ASSERT(backup->cluster.backup_primary_node_id == 0U);
    TEST_ASSERT(backup->cluster.membership_sequence == 0U);
    /* takeover_active survives the detach (Current legacy shape, e.5). */
    TEST_ASSERT(backup->cluster.backup_takeover_active == true);
    TEST_ASSERT(test_derive_phase(&backup->cluster, network.now_ms) ==
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
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
    size_t index;
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
    /* CLV2-06-01: the existing v3 Join producer now writes explicit legacy
     * metadata, but these values remain observational in this milestone. */
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        const ucn_cluster_member_t *member =
            &network.nodes[0].cluster.primary_members.slots[index];

        if (!member->occupied) {
            continue;
        }
        TEST_ASSERT(ucn_cluster_member_record_is_valid(member));
        TEST_ASSERT(member->status == UCN_CLUSTER_MEMBER_STATUS_COMMITTED);
        TEST_ASSERT(member->voting);
        TEST_ASSERT(member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V3);
        TEST_ASSERT(member->capabilities == 0U);
        TEST_ASSERT(member->joined_at_ms <= now_ms);
        TEST_ASSERT(member->last_keepalive_at_ms <= now_ms);
    }
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
    /* The existing Backup snapshot/delta producer uses the same bridge; a
     * failover must not silently re-create an unclassified mirror record. */
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        const ucn_cluster_member_t *member =
            &network.nodes[1].cluster.primary_members.slots[index];

        if (!member->occupied) {
            continue;
        }
        TEST_ASSERT(ucn_cluster_member_record_is_valid(member));
        TEST_ASSERT(member->status == UCN_CLUSTER_MEMBER_STATUS_COMMITTED);
        TEST_ASSERT(member->voting);
        TEST_ASSERT(member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V3);
        TEST_ASSERT(member->capabilities == 0U);
    }
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

    /* Scenario C (CLV2-01-04f): a recovery-eligible node (RECOVERY_OBSERVE:
     * role DETACHED + eligible + no backoff) accepts the same offer ->
     * RECOVERY_* -> JOIN_PENDING through the single entry point, shadow
     * committed FIRST with reason JOIN_INITIATED, then the site field
     * payload.  The mirror is aligned so the end-of-RX sync sees no phase
     * change and mints nothing. */
    recovery = &network.nodes[3];
    recovery->cluster.role = UCN_CLUSTER_ROLE_DETACHED;
    recovery->cluster.recovery_eligible = true;
    recovery->cluster.recovery_backoff_deadline_ms = 0U;
    recovery->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_OBSERVE;
    recovery->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    recovery->cluster.shadow_transition_count = 0U;
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
    TEST_ASSERT(recovery->cluster.shadow_transition_count == 1U);

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

/* CLV2-01-04c.2: consider_head_offer()'s same-cluster same-term Head
 * offer refresh path: a MEMBER in takeover grace performs the
 * MEMBER_TAKEOVER_GRACE -> MEMBER_ACTIVE transition through the single
 * entry point (reason HEAD_LEASE_RENEWED) BEFORE the site's lease
 * refresh + grace=0 writes; a MEMBER_ACTIVE node performs NO transition
 * (the grace=0 write stays a no-op); a shadow/legacy mismatch fails
 * closed and the lease is NOT refreshed. */
static int cluster_test_member_offer_grace_refresh_wiring(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *member;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    member = &network.nodes[2]; /* node 3: a MEMBER of Head node 1 */
    member->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    member->cluster.cluster_id = 1U;
    member->cluster.term = 1U;
    member->cluster.head_node_id = 1U;
    member->cluster.current_head_score = 9000U;
    member->cluster.head_lease_expires_at_ms = 110U;
    member->cluster.role_since_ms = 90U;

    /* Scenario A (required): the member is in takeover grace and the
     * same-cluster same-term Head offer refreshes the lease -> the GRACE
     * -> MEMBER_ACTIVE transition is committed FIRST (reason
     * HEAD_LEASE_RENEWED), then the site writes run in original order
     * (lease refresh + grace=0) and the grace is cleared. */
    member->cluster.head_grace_deadline_ms = 150U;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE;
    TEST_ASSERT(test_derive_phase(&member->cluster, 100U) ==
                UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE);

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = 1U;
    message.head_score = 9000U;
    message.available_capacity = 3U;
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, 1U, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(member->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(member->cluster.transition_reason ==
                UCN_CLUSTER_REASON_HEAD_LEASE_RENEWED);
    TEST_ASSERT(member->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(member->cluster.head_grace_deadline_ms == 0U);
    /* Lease refreshed: now 100 + config.lease_ms 40 == 140. */
    TEST_ASSERT(member->cluster.head_lease_expires_at_ms == 140U);
    TEST_ASSERT(member->cluster.current_head_score == 9000U);
    TEST_ASSERT(test_derive_phase(&member->cluster, 100U) ==
                UCN_CLUSTER_PHASE_MEMBER_ACTIVE);

    /* Scenario B: an already MEMBER_ACTIVE node performs NO transition -
     * the refresh keeps the shadow MEMBER_ACTIVE and the grace=0 write is
     * a no-op. */
    member->cluster.shadow_transition_count = 0U;
    member->cluster.transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    member->cluster.head_grace_deadline_ms = 0U;
    member->cluster.head_lease_expires_at_ms = 120U;
    message.nonce = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, 1U, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(member->cluster.shadow_transition_count == 0U);
    TEST_ASSERT(member->cluster.head_lease_expires_at_ms == 140U);
    TEST_ASSERT(member->cluster.head_grace_deadline_ms == 0U);

    /* Scenario C (fail closed): a member whose shadow mirror is stale
     * (MEMBER_ACTIVE while the legacy state derives GRACE) rejects the
     * transition - the lease is NOT refreshed and the grace is NOT
     * cleared; the end-of-RX shadow sync then reconciles the mirror. */
    {
        cluster_test_network_t net2;

        TEST_ASSERT(cluster_test_network_init(&net2, 3U) == 0);
        net2.now_ms = 100U;
        net2.nodes[2].cluster.role = UCN_CLUSTER_ROLE_MEMBER;
        net2.nodes[2].cluster.cluster_id = 1U;
        net2.nodes[2].cluster.term = 1U;
        net2.nodes[2].cluster.head_node_id = 1U;
        net2.nodes[2].cluster.current_head_score = 9000U;
        net2.nodes[2].cluster.head_lease_expires_at_ms = 110U;
        net2.nodes[2].cluster.head_grace_deadline_ms = 150U;
        net2.nodes[2].cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
        ucn_cluster_test_transition_asserts_set(false);
        message.nonce = 3U;
        TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        TEST_ASSERT(ucn_cluster_receive(&net2.nodes[2].cluster, 1U, true,
                                        encoded, sizeof(encoded)) == UCN_OK);
        ucn_cluster_test_transition_asserts_set(true);
        TEST_ASSERT(net2.nodes[2].cluster.role == UCN_CLUSTER_ROLE_MEMBER);
        TEST_ASSERT(net2.nodes[2].cluster.head_lease_expires_at_ms == 110U);
        TEST_ASSERT(net2.nodes[2].cluster.head_grace_deadline_ms == 150U);
        TEST_ASSERT(net2.nodes[2].cluster.shadow_phase ==
                    UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE);
    }
    return 0;
}

/* CLV2-M03 (03-05): a Member must never use score/Node-ID to switch from
 * Head A/term T to another Head B/term T.  Both MEMBER_ACTIVE and grace
 * enter the local-only TERM_CONFLICT wait; no LEAVE, JOIN or Head-switch
 * bookkeeping is emitted before a strictly higher Term is observed. */
static int cluster_test_member_same_term_conflict_blocks_score_switch(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *member;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    size_t index;
    size_t leave_count;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    member = &network.nodes[2]; /* node 3: MEMBER of old Head node 4 */
    member->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    member->cluster.cluster_id = 1U;
    member->cluster.term = 1U;
    member->cluster.head_node_id = 4U;
    member->cluster.current_head_score = 2000U;
    member->cluster.head_lease_expires_at_ms = 140U;
    member->cluster.role_since_ms = 0U;

    /* Scenario A: even after all former score samples, a competing A/1 Head
     * only enters TERM_CONFLICT_WAIT. */
    member->cluster.head_grace_deadline_ms = 0U;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = 2U; /* better Head: node 2 */
    message.head_score = 7000U;
    message.available_capacity = 3U;
    message.lease_ms = 8000U;
    for (index = 0U; index < 3U; ++index) {
        message.nonce = (uint32_t)(index + 1U);
        TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        TEST_ASSERT(ucn_cluster_receive(&member->cluster, 2U, true,
                                        encoded, sizeof(encoded)) == UCN_OK);
    }
    TEST_ASSERT(member->cluster.role == UCN_CLUSTER_ROLE_TERM_CONFLICT);
    TEST_ASSERT(member->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT);
    TEST_ASSERT(member->cluster.transition_reason ==
                UCN_CLUSTER_REASON_TERM_CONFLICT);
    TEST_ASSERT(member->cluster.stats.head_switches == 0U);
    TEST_ASSERT(member->cluster.pending_head_node_id == 0U);
    TEST_ASSERT(member->cluster.pending_cluster_id == 0U);
    TEST_ASSERT(member->cluster.pending_term == 0U);
    TEST_ASSERT(member->cluster.recovery_eligible == false);
    TEST_ASSERT(test_derive_phase(&member->cluster, 100U) ==
                UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT);
    /* Conflict handling is control-silent. */
    leave_count = 0U;
    for (index = 0U; index < network.queue_count; ++index) {
        if (network.queue[index].destination == 4U &&
            network.queue[index].payload[1U] ==
                (uint8_t)UCN_CLUSTER_MSG_LEAVE) {
            leave_count++;
        }
    }
    TEST_ASSERT(leave_count == 0U);
    TEST_ASSERT(network.queue_count == 0U);

    /* Scenario B: the same rule holds while the Member is in takeover grace. */
    {
        cluster_test_network_t net2;

        TEST_ASSERT(cluster_test_network_init(&net2, 3U) == 0);
        net2.now_ms = 100U;
        net2.nodes[2].cluster.role = UCN_CLUSTER_ROLE_MEMBER;
        net2.nodes[2].cluster.cluster_id = 1U;
        net2.nodes[2].cluster.term = 1U;
        net2.nodes[2].cluster.head_node_id = 4U;
        net2.nodes[2].cluster.current_head_score = 2000U;
        net2.nodes[2].cluster.head_lease_expires_at_ms = 140U;
        net2.nodes[2].cluster.head_grace_deadline_ms = 150U;
        net2.nodes[2].cluster.shadow_phase =
            UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE;
        for (index = 0U; index < 3U; ++index) {
            message.nonce = (uint32_t)(index + 10U);
            TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
            TEST_ASSERT(ucn_cluster_receive(&net2.nodes[2].cluster, 2U, true,
                                            encoded, sizeof(encoded)) == UCN_OK);
        }
        TEST_ASSERT(net2.nodes[2].cluster.role == UCN_CLUSTER_ROLE_TERM_CONFLICT);
        TEST_ASSERT(net2.nodes[2].cluster.shadow_phase ==
                    UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT);
        TEST_ASSERT(net2.nodes[2].cluster.transition_reason ==
                    UCN_CLUSTER_REASON_TERM_CONFLICT);
        TEST_ASSERT(net2.nodes[2].cluster.stats.head_switches == 0U);
        TEST_ASSERT(net2.nodes[2].cluster.pending_head_node_id == 0U);
        TEST_ASSERT(net2.nodes[2].cluster.pending_cluster_id == 0U);
        TEST_ASSERT(net2.nodes[2].cluster.pending_term == 0U);
        leave_count = 0U;
        for (index = 0U; index < net2.queue_count; ++index) {
            if (net2.queue[index].destination == 4U &&
                net2.queue[index].payload[1U] ==
                    (uint8_t)UCN_CLUSTER_MSG_LEAVE) {
                leave_count++;
            }
        }
        TEST_ASSERT(leave_count == 0U);
        TEST_ASSERT(net2.queue_count == 0U);
    }

    /* CLV2-M11 (11-08): a high-score foreign Head must be pure discovery
     * evidence for a Member.  Even repeated A/1 <- B/100 advertisements may
     * neither compare terms nor manufacture LEAVE/JOIN state. */
    {
        cluster_test_network_t foreign_network;
        cluster_test_node_t *foreign_member;

        TEST_ASSERT(cluster_test_network_init(&foreign_network, 3U) == 0);
        foreign_network.now_ms = 100U;
        foreign_member = &foreign_network.nodes[2];
        foreign_member->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
        foreign_member->cluster.cluster_id = 1U;
        foreign_member->cluster.term = 2U;
        foreign_member->cluster.head_node_id = 4U;
        foreign_member->cluster.current_head_score = 2000U;
        foreign_member->cluster.head_lease_expires_at_ms = 140U;
        foreign_member->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
        message.cluster_id = 9U;
        message.term = 100U;
        message.head_node_id = 2U;
        message.head_score = 7000U;
        for (index = 0U; index < 3U; ++index) {
            message.nonce = (uint32_t)(index + 30U);
            TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
            TEST_ASSERT(ucn_cluster_receive(&foreign_member->cluster, 2U, true,
                                            encoded, sizeof(encoded)) == UCN_OK);
        }
        TEST_ASSERT(foreign_member->cluster.role == UCN_CLUSTER_ROLE_MEMBER &&
                    foreign_member->cluster.cluster_id == 1U &&
                    foreign_member->cluster.term == 2U &&
                    foreign_member->cluster.head_node_id == 4U &&
                    foreign_member->cluster.current_head_score == 2000U &&
                    foreign_member->cluster.pending_cluster_id == 0U &&
                    foreign_member->cluster.stats.head_switches == 0U &&
                    foreign_network.queue_count == 0U);
    }
    return 0;
}

/* CLV2-01-04e: takeover-lifecycle wiring (e.3 start_takeover, e.4
 * complete_takeover, e.5 takeover timeout, e.6 handle_head_takeover).
 *
 *  (a) e.3: a READY Backup whose Primary lease lapsed commits
 *      BACKUP_READY -> BACKUP_TAKEOVER (TAKEOVER_STARTED) UNCONDITIONALLY
 *      first; takeover_active/deadline/ack bookkeeping are the site's own
 *      writes in order and ready/syncing stay untouched (CLV2-M01.0.2).
 *  (b) the reachable takeover_active && syncing combo stays expressible
 *      after start_takeover (late same-generation Type12 re-arms syncing).
 *  (c) e.4: the quorum commits BACKUP_TAKEOVER -> HEAD_NO_BACKUP
 *      (TAKEOVER_QUORUM) first, then the FULL Current clear set runs at
 *      the site (every field asserted); backup_generation is untouched.
 *  (d) e.4 shadow-desync fails closed: NOTHING of the clear set runs.
 *  (e) e.5: the expired takeover window commits BACKUP_TAKEOVER ->
 *      DETACHED_OBSERVE (TAKEOVER_TIMEOUT) first, then stats +
 *      backup_clear_sync; recovery_eligible is NOT set (matching Current).
 *  (f) e.6: HEAD_TAKEOVER from BACKUP_READY / BACKUP_TAKEOVER / GRACE
 *      commits -> MEMBER_ACTIVE (TAKEOVER_STARTED) with the FULL site
 *      clear set + epoch refresh (F1 anchor).
 *  (g) shadow-desync variants for each site fail closed with zero writes. */
static int cluster_test_takeover_lifecycle_wiring(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_t *c;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    uint32_t baseline_won;
    uint32_t baseline_switches;
    uint32_t before;

    /* ============ (a) e.3 start_takeover: BACKUP_READY -> BACKUP_TAKEOVER ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[1]; /* node 2: the READY Backup of Head node 1 */
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = 1U;
    c->backup_primary_node_id = 1U;
    c->backup_generation = 7U;
    c->backup_ready = true;
    c->backup_syncing = false;
    c->backup_takeover_active = false;
    c->backup_takeover_deadline_ms = 0U;
    c->backup_takeover_ack_count = 0U;
    c->backup_takeover_acked = 0U;
    c->backup_takeover_prepare_cursor = 0U;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    /* The mirror contains the Backup itself (C07.7 P1 self-vote path). */
    (void)memset(c->primary_members.slots, 0, sizeof(c->primary_members.slots));
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = node->node_id;
    c->primary_members.slots[1].occupied = true;
    c->primary_members.slots[1].node_id = 3U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_BACKUP_READY);
    TEST_ASSERT(ucn_cluster_test_start_takeover(c, 100U) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_TAKEOVER_STARTED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(c->backup_takeover_active == true);
    TEST_ASSERT(c->backup_takeover_deadline_ms == ucn_deadline_from_now(
                    100U, UCN_CLUSTER_TAKEOVER_WINDOW_MS));
    TEST_ASSERT(c->backup_takeover_ack_count == 1U); /* self vote */
    /* (b) M01.0.2: start_takeover never touches ready/syncing; a late
     * same-generation Type12 can then re-arm syncing while takeover is
     * active and the combo stays expressible (BACKUP_TAKEOVER derives). */
    TEST_ASSERT(c->backup_ready == true);
    TEST_ASSERT(c->backup_syncing == false);
    c->backup_syncing = true; /* delayed SYNC_BEGIN re-arms syncing */
    c->backup_ready = false;
    TEST_ASSERT(test_legacy_state_valid(c) == true);
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    TEST_ASSERT(c->backup_takeover_active == true);
    TEST_ASSERT(c->backup_syncing == true);

    /* (g) e.3 shadow-desync: stale shadow (BACKUP_TAKEOVER) while the
     * legacy derives BACKUP_READY -> fail closed, ZERO site writes. */
    {
        cluster_test_network_t net2;

        TEST_ASSERT(cluster_test_network_init(&net2, 3U) == 0);
        net2.now_ms = 100U;
        net2.nodes[1].cluster.role = UCN_CLUSTER_ROLE_BACKUP;
        net2.nodes[1].cluster.backup_ready = true;
        net2.nodes[1].cluster.backup_syncing = false;
        net2.nodes[1].cluster.backup_takeover_active = false;
        net2.nodes[1].cluster.backup_takeover_ack_count = 0U;
        net2.nodes[1].cluster.backup_takeover_deadline_ms = 0U;
        net2.nodes[1].cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
        ucn_cluster_test_transition_asserts_set(false);
        TEST_ASSERT(ucn_cluster_test_start_takeover(
                        &net2.nodes[1].cluster, 100U) == UCN_ERR_STATE);
        ucn_cluster_test_transition_asserts_set(true);
        TEST_ASSERT(net2.nodes[1].cluster.role == UCN_CLUSTER_ROLE_BACKUP);
        TEST_ASSERT(net2.nodes[1].cluster.backup_takeover_active == false);
        TEST_ASSERT(net2.nodes[1].cluster.backup_takeover_ack_count == 0U);
        TEST_ASSERT(net2.nodes[1].cluster.backup_takeover_deadline_ms == 0U);
        TEST_ASSERT(net2.nodes[1].cluster.shadow_phase ==
                    UCN_CLUSTER_PHASE_BACKUP_TAKEOVER); /* untouched */
    }

    /* ============ (c) e.4 complete_takeover: BACKUP_TAKEOVER -> HEAD_NO_BACKUP ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[1]; /* node 2 */
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = 1U;
    c->current_head_score = 9000U;
    c->backup_primary_node_id = 1U;
    c->backup_generation = 7U;
    c->backup_ready = false;
    c->backup_syncing = false;
    c->backup_takeover_active = true;
    c->backup_takeover_deadline_ms = 1100U;
    c->backup_takeover_ack_count = 2U;
    c->backup_takeover_acked = 3U;
    c->backup_takeover_prepare_cursor = 1U;
    c->backup_takeover_announce_cursor = 9U;
    c->backup_takeover_announce_remaining = 9U;
    c->backup_takeover_announce_active = true;
    c->known_backup_node_id = 1U;
    c->known_backup_generation = 7U;
    c->election_deadline_ms = 200U;
    c->next_advertise_ms = 200U;
    c->role_since_ms = 90U;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    (void)memset(c->primary_members.slots, 0, sizeof(c->primary_members.slots));
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = node->node_id; /* self */
    c->primary_members.slots[0].lease_expires_at_ms = 50U;
    c->primary_members.slots[0].last_nonce = 11U;
    c->primary_members.slots[1].occupied = true;
    c->primary_members.slots[1].node_id = 3U;
    c->primary_members.slots[1].lease_expires_at_ms = 60U;
    c->primary_members.slots[1].last_nonce = 22U;
    baseline_won = c->stats.elections_won;
    baseline_switches = c->stats.head_switches;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    TEST_ASSERT(ucn_cluster_test_complete_takeover(c, 100U) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_TAKEOVER_QUORUM);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    /* The FULL Current clear set, field by field (F1 anchor). */
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_HEAD);
    TEST_ASSERT(c->term == 6U); /* 5 + 1 */
    TEST_ASSERT(c->head_node_id == node->node_id);
    TEST_ASSERT(c->current_head_score == c->config.head_score);
    TEST_ASSERT(c->role_since_ms == 100U);
    TEST_ASSERT(c->election_deadline_ms == 0U);
    TEST_ASSERT(c->next_advertise_ms == 100U);
    TEST_ASSERT(c->backup_takeover_active == false);
    TEST_ASSERT(c->backup_syncing == false);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->backup_node_id == 0U);
    TEST_ASSERT(c->backup_primary_node_id == 0U);
    TEST_ASSERT(c->known_backup_node_id == 0U);
    TEST_ASSERT(c->known_backup_generation == 0U);
    TEST_ASSERT(c->stats.elections_won == baseline_won + 1U);
    TEST_ASSERT(c->stats.head_switches == baseline_switches + 1U);
    /* backup_generation is caller-owned and untouched (review C G1). */
    TEST_ASSERT(c->backup_generation == 7U);
    /* The new Head is not its own member; the peer lease is renewed. */
    TEST_ASSERT(c->primary_members.slots[0].occupied == false);
    TEST_ASSERT(c->primary_members.slots[1].occupied == true);
    TEST_ASSERT(c->primary_members.slots[1].lease_expires_at_ms == ucn_deadline_from_now(
                    100U, c->config.lease_ms));
    /* Announce bookkeeping reset to the post-clear mirror count. */
    TEST_ASSERT(c->backup_takeover_announce_cursor == 0U);
    TEST_ASSERT(c->backup_takeover_announce_remaining == 1U);
    TEST_ASSERT(c->backup_takeover_announce_active == true);
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);

    /* (d) e.4 shadow-desync: stale shadow (DETACHED_OBSERVE) while the
     * legacy derives BACKUP_TAKEOVER -> fail closed, NOTHING cleared
     * (mirror-symmetric to (c)). */
    {
        cluster_test_network_t net2;

        TEST_ASSERT(cluster_test_network_init(&net2, 3U) == 0);
        net2.now_ms = 100U;
        net2.nodes[1].cluster.role = UCN_CLUSTER_ROLE_BACKUP;
        net2.nodes[1].cluster.term = 5U;
        net2.nodes[1].cluster.head_node_id = 1U;
        net2.nodes[1].cluster.backup_takeover_active = true;
        net2.nodes[1].cluster.backup_syncing = true; /* M01.0.2 combo */
        net2.nodes[1].cluster.backup_ready = false;
        net2.nodes[1].cluster.backup_primary_node_id = 1U;
        net2.nodes[1].cluster.backup_generation = 7U;
        net2.nodes[1].cluster.known_backup_node_id = 1U;
        net2.nodes[1].cluster.known_backup_generation = 7U;
        net2.nodes[1].cluster.shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE;
        ucn_cluster_test_transition_asserts_set(false);
        TEST_ASSERT(ucn_cluster_test_complete_takeover(
                        &net2.nodes[1].cluster, 100U) == UCN_ERR_STATE);
        ucn_cluster_test_transition_asserts_set(true);
        TEST_ASSERT(net2.nodes[1].cluster.role == UCN_CLUSTER_ROLE_BACKUP);
        TEST_ASSERT(net2.nodes[1].cluster.term == 5U);
        TEST_ASSERT(net2.nodes[1].cluster.head_node_id == 1U);
        TEST_ASSERT(net2.nodes[1].cluster.backup_takeover_active == true);
        TEST_ASSERT(net2.nodes[1].cluster.backup_syncing == true);
        TEST_ASSERT(net2.nodes[1].cluster.backup_ready == false);
        TEST_ASSERT(net2.nodes[1].cluster.backup_primary_node_id == 1U);
        TEST_ASSERT(net2.nodes[1].cluster.backup_generation == 7U);
        TEST_ASSERT(net2.nodes[1].cluster.known_backup_node_id == 1U);
        TEST_ASSERT(net2.nodes[1].cluster.known_backup_generation == 7U);
        TEST_ASSERT(net2.nodes[1].cluster.stats.elections_won == 0U);
        TEST_ASSERT(net2.nodes[1].cluster.stats.head_switches == 0U);
        TEST_ASSERT(net2.nodes[1].cluster.shadow_phase ==
                    UCN_CLUSTER_PHASE_DETACHED_OBSERVE); /* untouched */
    }

    /* ============ (e) e.5 takeover timeout: BACKUP_TAKEOVER -> DETACHED_OBSERVE ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 1U) == 0);
    network.now_ms = 100U;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = 1U;
    c->backup_primary_node_id = 1U;
    c->backup_generation = 7U;
    c->backup_ready = false;
    c->backup_syncing = true; /* M01.0.2 combo survives into the timeout */
    c->backup_takeover_active = true;
    c->backup_takeover_deadline_ms = 10U; /* expired at now == 100 */
    c->backup_primary_deadline_ms = 0U;   /* skip the missed-heartbeat branch */
    c->known_backup_node_id = 1U;
    c->known_backup_generation = 7U;
    c->recovery_eligible = false;
    c->membership_sequence = 4U;
    c->backup_missed_heartbeats = 2U;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    (void)memset(c->primary_members.slots, 0, sizeof(c->primary_members.slots));
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = 2U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    before = c->stats.head_leases_expired;
    TEST_ASSERT(ucn_cluster_step(c) == UCN_OK);
    TEST_ASSERT(c->stats.head_leases_expired == before + 1U);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_TAKEOVER_TIMEOUT);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_DETACHED);
    /* recovery_eligible is NOT set (a takeover-active Backup is never
     * recovery-eligible - matching Current). */
    TEST_ASSERT(c->recovery_eligible == false);
    /* backup_clear_sync() site effects in original order. */
    TEST_ASSERT(c->backup_syncing == false);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->backup_primary_node_id == 0U);
    TEST_ASSERT(c->backup_generation == 0U);
    TEST_ASSERT(c->membership_sequence == 0U);
    TEST_ASSERT(c->backup_primary_deadline_ms == 0U);
    TEST_ASSERT(c->backup_missed_heartbeats == 0U);
    TEST_ASSERT(c->primary_members.slots[0].occupied == false);
    TEST_ASSERT(c->known_backup_node_id == 0U);
    TEST_ASSERT(c->known_backup_generation == 0U);
    TEST_ASSERT(c->head_grace_deadline_ms == 0U);
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_DETACHED_OBSERVE);

    /* (g) e.5 shadow-desync: stale shadow (BACKUP_READY) while the legacy
     * derives BACKUP_TAKEOVER -> step fails closed, NOTHING cleared. */
    {
        cluster_test_network_t net2;

        TEST_ASSERT(cluster_test_network_init(&net2, 1U) == 0);
        net2.now_ms = 100U;
        net2.nodes[0].cluster.role = UCN_CLUSTER_ROLE_BACKUP;
        net2.nodes[0].cluster.backup_takeover_active = true;
        net2.nodes[0].cluster.backup_takeover_deadline_ms = 10U;
        net2.nodes[0].cluster.backup_primary_deadline_ms = 0U;
        net2.nodes[0].cluster.backup_syncing = true;
        net2.nodes[0].cluster.backup_ready = false;
        net2.nodes[0].cluster.backup_generation = 7U;
        net2.nodes[0].cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;
        ucn_cluster_test_transition_asserts_set(false);
        TEST_ASSERT(ucn_cluster_step(&net2.nodes[0].cluster) == UCN_ERR_STATE);
        ucn_cluster_test_transition_asserts_set(true);
        TEST_ASSERT(net2.nodes[0].cluster.role == UCN_CLUSTER_ROLE_BACKUP);
        TEST_ASSERT(net2.nodes[0].cluster.backup_takeover_active == true);
        TEST_ASSERT(net2.nodes[0].cluster.backup_syncing == true);
        TEST_ASSERT(net2.nodes[0].cluster.backup_generation == 7U);
        TEST_ASSERT(net2.nodes[0].cluster.shadow_phase ==
                    UCN_CLUSTER_PHASE_BACKUP_READY); /* untouched (no sync on error) */
    }

    /* ============ (f) e.6 handle_head_takeover: BACKUP_READY -> MEMBER_ACTIVE ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2]; /* node 3: the READY Backup of Head node 1 */
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = 1U;
    c->current_head_score = 9000U;
    c->backup_primary_node_id = 1U;
    c->backup_generation = 7U;
    c->backup_ready = true;
    c->backup_syncing = false;
    c->backup_takeover_active = false;
    c->known_backup_node_id = 2U; /* the announcing new Head */
    c->known_backup_generation = 7U;
    c->head_lease_expires_at_ms = 150U;
    c->head_grace_deadline_ms = 0U;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_BACKUP_READY);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_TAKEOVER;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 6U;
    message.head_node_id = 2U;
    message.head_score = 7000U;
    message.lease_ms = 8000U;
    message.backup_generation = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, 2U, true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_TAKEOVER_STARTED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->cluster_id == 1U);
    TEST_ASSERT(c->term == 6U);
    TEST_ASSERT(c->head_node_id == 2U);
    TEST_ASSERT(c->current_head_score == 7000U);
    TEST_ASSERT(c->head_lease_expires_at_ms == ucn_deadline_from_now(
                    100U, 8000U));
    TEST_ASSERT(c->head_grace_deadline_ms == 0U);
    TEST_ASSERT(c->next_keepalive_ms == 100U);
    TEST_ASSERT(c->pending_head_node_id == 0U);
    TEST_ASSERT(c->pending_cluster_id == 0U);
    TEST_ASSERT(c->pending_term == 0U);
    /* F1 anchor: the full clear set stayed AT THE SITE. */
    TEST_ASSERT(c->backup_takeover_active == false);
    TEST_ASSERT(c->backup_syncing == false);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->known_backup_node_id == 0U);
    TEST_ASSERT(c->known_backup_generation == 0U);
    TEST_ASSERT(c->recovery_eligible == false);
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);

    /* (f2) e.6 BACKUP_TAKEOVER inbound (with the M01.0.2 combo): the
     * higher-Term Head announcement switches the takeover-active Backup
     * to MEMBER_ACTIVE and clears takeover/syncing/ready/known_backup. */
    {
        cluster_test_network_t net2;

        TEST_ASSERT(cluster_test_network_init(&net2, 3U) == 0);
        net2.now_ms = 100U;
        net2.nodes[2].cluster.role = UCN_CLUSTER_ROLE_BACKUP;
        net2.nodes[2].cluster.cluster_id = 1U;
        net2.nodes[2].cluster.term = 5U;
        net2.nodes[2].cluster.head_node_id = 1U;
        net2.nodes[2].cluster.backup_takeover_active = true;
        net2.nodes[2].cluster.backup_syncing = true;
        net2.nodes[2].cluster.backup_ready = false;
        net2.nodes[2].cluster.backup_primary_node_id = 1U;
        net2.nodes[2].cluster.backup_generation = 7U;
        net2.nodes[2].cluster.known_backup_node_id = 2U;
        net2.nodes[2].cluster.known_backup_generation = 7U;
        net2.nodes[2].cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
        net2.nodes[2].cluster.transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
        net2.nodes[2].cluster.shadow_transition_count = 0U;
        message.nonce = 0U;
        TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        TEST_ASSERT(ucn_cluster_receive(&net2.nodes[2].cluster, 2U, true,
                                        encoded, sizeof(encoded)) == UCN_OK);
        TEST_ASSERT(net2.nodes[2].cluster.role == UCN_CLUSTER_ROLE_MEMBER);
        TEST_ASSERT(net2.nodes[2].cluster.shadow_phase ==
                    UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
        TEST_ASSERT(net2.nodes[2].cluster.transition_reason ==
                    UCN_CLUSTER_REASON_TAKEOVER_STARTED);
        TEST_ASSERT(net2.nodes[2].cluster.shadow_transition_count == 1U);
        TEST_ASSERT(net2.nodes[2].cluster.term == 6U);
        TEST_ASSERT(net2.nodes[2].cluster.head_node_id == 2U);
        TEST_ASSERT(net2.nodes[2].cluster.backup_takeover_active == false);
        TEST_ASSERT(net2.nodes[2].cluster.backup_syncing == false);
        TEST_ASSERT(net2.nodes[2].cluster.backup_ready == false);
        TEST_ASSERT(net2.nodes[2].cluster.known_backup_node_id == 0U);
        TEST_ASSERT(net2.nodes[2].cluster.known_backup_generation == 0U);
        TEST_ASSERT(test_derive_phase(&net2.nodes[2].cluster, 100U) ==
                    UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    }

    /* (f3) e.6 GRACE inbound: a MEMBER in takeover grace switches to
     * MEMBER_ACTIVE with the same epoch refresh + clears. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2]; /* node 3: a MEMBER in takeover grace */
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_MEMBER;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = 1U;
    c->current_head_score = 9000U;
    c->known_backup_node_id = 2U;
    c->known_backup_generation = 7U;
    c->head_lease_expires_at_ms = 90U; /* lease already lapsed */
    c->head_grace_deadline_ms = 150U;  /* armed: the node is IN grace */
    c->backup_takeover_active = false;
    c->backup_syncing = false;
    c->backup_ready = false;
    c->shadow_phase = UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) ==
                UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE);
    message.nonce = 0U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, 2U, true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_TAKEOVER_STARTED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->term == 6U);
    TEST_ASSERT(c->head_node_id == 2U);
    TEST_ASSERT(c->head_grace_deadline_ms == 0U);
    TEST_ASSERT(c->head_lease_expires_at_ms == ucn_deadline_from_now(
                    100U, 8000U));
    TEST_ASSERT(c->backup_takeover_active == false);
    TEST_ASSERT(c->backup_syncing == false);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->known_backup_node_id == 0U);
    TEST_ASSERT(c->known_backup_generation == 0U);
    TEST_ASSERT(c->recovery_eligible == false);
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);

    /* (f4) e.6 BACKUP_SYNCING inbound (review C MINOR 1): a syncing (not
     * yet READY) Backup switched by the higher-Term Head announcement
     * must take the same MEMBER_ACTIVE transition and the full clear set
     * - the else-of-the-ternary edge of handle_head_takeover (derive
     * old=BACKUP_SYNCING for role==BACKUP && !takeover && !ready), which
     * no other sub-case covers. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2]; /* node 3: a syncing Backup of Head node 1 */
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = 1U;
    c->current_head_score = 9000U;
    c->backup_primary_node_id = 1U;
    c->backup_generation = 7U;
    c->backup_ready = false;
    c->backup_syncing = true;
    c->backup_takeover_active = false;
    c->known_backup_node_id = 2U; /* the announcing new Head */
    c->known_backup_generation = 7U;
    c->head_lease_expires_at_ms = 150U;
    c->head_grace_deadline_ms = 0U;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_SYNCING;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    message.nonce = 0U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, 2U, true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_TAKEOVER_STARTED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->cluster_id == 1U);
    TEST_ASSERT(c->term == 6U);
    TEST_ASSERT(c->head_node_id == 2U);
    TEST_ASSERT(c->current_head_score == 7000U);
    TEST_ASSERT(c->head_lease_expires_at_ms == ucn_deadline_from_now(
                    100U, 8000U));
    TEST_ASSERT(c->head_grace_deadline_ms == 0U);
    TEST_ASSERT(c->next_keepalive_ms == 100U);
    /* F1 anchor: the full clear set stayed AT THE SITE. */
    TEST_ASSERT(c->backup_takeover_active == false);
    TEST_ASSERT(c->backup_syncing == false);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->known_backup_node_id == 0U);
    TEST_ASSERT(c->known_backup_generation == 0U);
    TEST_ASSERT(c->recovery_eligible == false);
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);

    /* (g) e.6 shadow-desync: stale shadow (DETACHED_OBSERVE) while the
     * legacy derives BACKUP_READY -> receive fails closed, NOTHING
     * cleared and NO epoch refresh; the end-of-RX sync re-aligns the
     * mirror to the still-derived BACKUP_READY. */
    {
        cluster_test_network_t net2;

        TEST_ASSERT(cluster_test_network_init(&net2, 3U) == 0);
        net2.now_ms = 100U;
        net2.nodes[2].cluster.role = UCN_CLUSTER_ROLE_BACKUP;
        net2.nodes[2].cluster.cluster_id = 1U;
        net2.nodes[2].cluster.term = 5U;
        net2.nodes[2].cluster.head_node_id = 1U;
        net2.nodes[2].cluster.backup_ready = true;
        net2.nodes[2].cluster.backup_syncing = false;
        net2.nodes[2].cluster.backup_takeover_active = false;
        net2.nodes[2].cluster.known_backup_node_id = 2U;
        net2.nodes[2].cluster.known_backup_generation = 7U;
        net2.nodes[2].cluster.shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE;
        message.nonce = 0U;
        TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        ucn_cluster_test_transition_asserts_set(false);
        TEST_ASSERT(ucn_cluster_receive(&net2.nodes[2].cluster, 2U, true,
                                        encoded, sizeof(encoded)) == UCN_ERR_STATE);
        ucn_cluster_test_transition_asserts_set(true);
        TEST_ASSERT(net2.nodes[2].cluster.role == UCN_CLUSTER_ROLE_BACKUP);
        TEST_ASSERT(net2.nodes[2].cluster.term == 5U);
        TEST_ASSERT(net2.nodes[2].cluster.head_node_id == 1U);
        TEST_ASSERT(net2.nodes[2].cluster.backup_ready == true);
        TEST_ASSERT(net2.nodes[2].cluster.backup_takeover_active == false);
        TEST_ASSERT(net2.nodes[2].cluster.known_backup_node_id == 2U);
        TEST_ASSERT(net2.nodes[2].cluster.shadow_phase ==
                    UCN_CLUSTER_PHASE_BACKUP_READY); /* re-aligned, no mint */
    }
    return 0;
}

/* CLV2-01-04d.5: backup_resync() wiring - a HEAD_STABLE node restarting
 * its Backup snapshot performs the HEAD_STABLE -> HEAD_BACKUP_SYNCING
 * transition through the single entry point (reason RESYNC_STARTED)
 * BEFORE the site's ready=false write; a node that is already SYNCING
 * (ready cleared first by remove_member()/expire_members(), or a resync
 * already in flight) runs NO transition - the legacy body alone re-arms
 * the snapshot. */
static int cluster_test_resync_transition_wiring(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 0U;
    head = &network.nodes[0];

    /* (a) HEAD_STABLE -> HEAD_BACKUP_SYNCING via BACKUP_RESYNC_REQ:
     * the transition commits FIRST (shadow + EXPLICIT RESYNC_STARTED),
     * then the site re-arms the snapshot in its original order. */
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = network.nodes[1].node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.backup_ready = true;
    head->cluster.backup_sync_cursor = 5U;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_STABLE;
    head->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    head->cluster.shadow_transition_count = 0U;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ;
    message.role = UCN_CLUSTER_ROLE_BACKUP;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, network.nodes[1].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
    TEST_ASSERT(head->cluster.transition_reason ==
                UCN_CLUSTER_REASON_RESYNC_STARTED);
    TEST_ASSERT(head->cluster.shadow_transition_count == 1U);
    /* site effects in original order: cursor reset, ready=false, re-arm. */
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_HEAD);
    TEST_ASSERT(head->cluster.backup_node_id == network.nodes[1].node_id);
    TEST_ASSERT(head->cluster.backup_sync_cursor == 0U);
    TEST_ASSERT(head->cluster.backup_ready == false);
    TEST_ASSERT(head->cluster.next_backup_sync_ms == 0U);
    TEST_ASSERT(head->cluster.backup_resync_deadline_ms != 0U);

    /* (b) already SYNCING (ready=false): NO transition - the legacy body
     * alone re-arms the snapshot, exactly as before the migration. */
    head->cluster.backup_sync_cursor = 7U;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
    head->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    head->cluster.shadow_transition_count = 0U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, network.nodes[1].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
    TEST_ASSERT(head->cluster.transition_reason == UCN_CLUSTER_REASON_INIT);
    TEST_ASSERT(head->cluster.shadow_transition_count == 0U);
    TEST_ASSERT(head->cluster.backup_sync_cursor == 0U);
    TEST_ASSERT(head->cluster.backup_ready == false);
    return 0;
}

/* CLV2-01-04d.6 / CLV2-M03: ordered stepdown wiring.  A real RX offer
 * from a proven higher-Term Head enters HEAD_* -> STEPPING_DOWN through
 * the global gate (HIGHER_AUTHORITY); the direct helper retains the
 * STEPDOWN_ORDERED reason for its explicit ordered-yield contract. */
static int cluster_test_head_stepdown_transition_wiring(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    cluster_test_node_t *recovery;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_cluster_candidate_t candidate;
    ucn_result_t result;
    uint32_t nonce;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 60U;
    head = &network.nodes[0];

    /* (c) A HEAD_STABLE node receives a same-Cluster higher-Term
     * HEAD_DECLARE.  The global gate commits HEAD_STABLE -> STEPPING_DOWN
     * FIRST (shadow + HIGHER_AUTHORITY), then the site
     * yields (eligible=false, deadline armed, pending Head identity). */
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.config.head_score = 100U;
    head->cluster.current_head_score = 100U;
    head->cluster.role_since_ms = 0U;
    head->cluster.backup_node_id = network.nodes[2].node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.backup_ready = true;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_STABLE;
    head->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    head->cluster.shadow_transition_count = 0U;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_DECLARE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 2U;
    message.head_node_id = network.nodes[1].node_id;
    message.head_score = 200U; /* improves by > 20% over 100 */
    message.available_capacity = 3U;
    message.lease_ms = 40U;
    for (nonce = 1U; nonce <= 3U; ++nonce) {
        message.nonce = nonce;
        TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        TEST_ASSERT(ucn_cluster_receive(&head->cluster,
                                        network.nodes[1].node_id, true,
                                        encoded, sizeof(encoded)) == UCN_OK);
    }
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_STEPPING_DOWN);
    TEST_ASSERT(head->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_STEPPING_DOWN);
    TEST_ASSERT(head->cluster.transition_reason ==
                UCN_CLUSTER_REASON_HIGHER_AUTHORITY);
    TEST_ASSERT(head->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(head->cluster.recovery_eligible == false);
    TEST_ASSERT(head->cluster.recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(head->cluster.stepdown_deadline_ms ==
                ucn_deadline_from_now(
                    network.now_ms,
                    head->cluster.config.keepalive_interval_ms));
    TEST_ASSERT(head->cluster.pending_head_node_id ==
                network.nodes[1].node_id);
    TEST_ASSERT(head->cluster.pending_cluster_id == 1U);
    TEST_ASSERT(head->cluster.pending_term == 2U);
    TEST_ASSERT(head->cluster.pending_head_score == 200U);
    TEST_ASSERT(head->cluster.stats.head_switches == 1U);
    /* The Head keeps its Backup selection through the ordered yield. */
    TEST_ASSERT(head->cluster.backup_node_id == network.nodes[2].node_id);
    TEST_ASSERT(head->cluster.backup_ready == true);

    /* CLV2-03-R07: a duplicate of the selected pending Head preserves the
     * ordered grace, but a still newer same-Cluster Term retargets it
     * immediately rather than waiting for the obsolete deadline. */
    message.term = 3U;
    message.nonce = 4U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, network.nodes[1].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(head->cluster.shadow_phase == UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(head->cluster.transition_reason ==
                UCN_CLUSTER_REASON_HIGHER_AUTHORITY);
    TEST_ASSERT(head->cluster.pending_term == 3U);

    /* (d) RECOVERY_HEAD + higher-Term Head offer (end-to-end RX): the
     * RECOVERY_HEAD -> STEPPING_DOWN transition commits FIRST through the
     * global gate (shadow + HIGHER_AUTHORITY + count), then the site yields (eligible=false,
     * backoff=0, stepdown deadline armed, pending Head identity,
     * head_switches).  The shadow is aligned (RECOVERY_HEAD) so the
     * end-of-RX sync sees no phase change and mints nothing. */
    recovery = &network.nodes[3];
    recovery->cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    recovery->cluster.cluster_id = 1U;
    recovery->cluster.term = 1U;
    recovery->cluster.head_node_id = recovery->node_id;
    recovery->cluster.recovery_eligible = true;
    recovery->cluster.recovery_backoff_deadline_ms = 0U;
    recovery->cluster.stepdown_deadline_ms = 0U;
    recovery->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    recovery->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    recovery->cluster.shadow_transition_count = 0U;
    message.term = 2U;
    message.nonce = 10U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&recovery->cluster,
                                    network.nodes[1].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(recovery->cluster.role == UCN_CLUSTER_ROLE_STEPPING_DOWN);
    TEST_ASSERT(recovery->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_STEPPING_DOWN);
    TEST_ASSERT(recovery->cluster.transition_reason ==
                UCN_CLUSTER_REASON_HIGHER_AUTHORITY);
    TEST_ASSERT(recovery->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(recovery->cluster.pending_head_node_id ==
                network.nodes[1].node_id);
    TEST_ASSERT(recovery->cluster.pending_cluster_id == 1U);
    TEST_ASSERT(recovery->cluster.pending_term == 2U);
    TEST_ASSERT(recovery->cluster.pending_head_score == 200U);
    TEST_ASSERT(recovery->cluster.recovery_eligible == false);
    TEST_ASSERT(recovery->cluster.recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(recovery->cluster.stepdown_deadline_ms ==
                ucn_deadline_from_now(60U,
                    recovery->cluster.config.keepalive_interval_ms));
    TEST_ASSERT(recovery->cluster.stats.head_switches == 1U);

    /* (e) RECOVERY_HEAD + stable-Head reclaim offer (direct hook, no
     * end-of-RX sync): the same explicit RECOVERY_HEAD -> STEPPING_DOWN
     * transition commits FIRST (shadow + STEPDOWN_ORDERED + count) - the
     * discriminator that would fail if the entry-point call were removed
     * (the legacy body alone cannot produce shadow/reason/count). */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 60U;
    recovery = &network.nodes[3];
    recovery->cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    recovery->cluster.cluster_id = 1U;
    recovery->cluster.term = 1U;
    recovery->cluster.head_node_id = recovery->node_id;
    recovery->cluster.recovery_eligible = true;
    recovery->cluster.recovery_backoff_deadline_ms = 0U;
    recovery->cluster.stepdown_deadline_ms = 0U;
    recovery->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    recovery->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    recovery->cluster.shadow_transition_count = 0U;
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.head_node_id = 2U; /* the stable reclaiming Head */
    candidate.cluster_id = 1U;
    candidate.term = 1U;
    candidate.head_score = 200U;
    TEST_ASSERT(test_derive_phase(&recovery->cluster, 60U) ==
                UCN_CLUSTER_PHASE_RECOVERY_HEAD);
    result = ucn_cluster_test_begin_ordered_stepdown(&recovery->cluster,
                                                     &candidate, 60U);
    TEST_ASSERT(result == UCN_OK);
    TEST_ASSERT(recovery->cluster.role == UCN_CLUSTER_ROLE_STEPPING_DOWN);
    TEST_ASSERT(recovery->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_STEPPING_DOWN);
    TEST_ASSERT(recovery->cluster.transition_reason ==
                UCN_CLUSTER_REASON_STEPDOWN_ORDERED);
    TEST_ASSERT(recovery->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(recovery->cluster.recovery_eligible == false);
    TEST_ASSERT(recovery->cluster.recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(recovery->cluster.stepdown_deadline_ms ==
                ucn_deadline_from_now(60U,
                    recovery->cluster.config.keepalive_interval_ms));
    TEST_ASSERT(recovery->cluster.pending_head_node_id == 2U);
    TEST_ASSERT(recovery->cluster.pending_cluster_id == 1U);
    TEST_ASSERT(recovery->cluster.pending_term == 1U);
    TEST_ASSERT(recovery->cluster.pending_head_score == 200U);
    TEST_ASSERT(recovery->cluster.stats.head_switches == 1U);
    TEST_ASSERT(test_derive_phase(&recovery->cluster, 60U) ==
                UCN_CLUSTER_PHASE_STEPPING_DOWN);

    /* (f) shadow-desync (CLV2-01-04f SITE B): a stale shadow fails closed
     * with UCN_ERR_STATE, ZERO site writes and no stepdown (recovery
     * fields + pending_* + stepdown deadline untouched) - driven via the
     * test hook so no end-of-RX sync can re-align the mirror afterwards. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 60U;
    recovery = &network.nodes[3];
    recovery->cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    recovery->cluster.cluster_id = 1U;
    recovery->cluster.term = 1U;
    recovery->cluster.head_node_id = recovery->node_id;
    recovery->cluster.recovery_eligible = true;
    recovery->cluster.recovery_backoff_deadline_ms = 0U;
    recovery->cluster.stepdown_deadline_ms = 0U;
    recovery->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_OBSERVE; /* stale */
    recovery->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    recovery->cluster.shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(&recovery->cluster, 60U) ==
                UCN_CLUSTER_PHASE_RECOVERY_HEAD);
    ucn_cluster_test_transition_asserts_set(false);
    result = ucn_cluster_test_begin_ordered_stepdown(&recovery->cluster,
                                                     &candidate, 60U);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(result == UCN_ERR_STATE);
    /* NOTHING of the stepdown site body ran (fail closed). */
    TEST_ASSERT(recovery->cluster.role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    TEST_ASSERT(recovery->cluster.recovery_eligible == true);
    TEST_ASSERT(recovery->cluster.recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(recovery->cluster.stepdown_deadline_ms == 0U);
    TEST_ASSERT(recovery->cluster.pending_head_node_id == 0U);
    TEST_ASSERT(recovery->cluster.pending_cluster_id == 0U);
    TEST_ASSERT(recovery->cluster.pending_term == 0U);
    TEST_ASSERT(recovery->cluster.pending_head_score == 0U);
    TEST_ASSERT(recovery->cluster.stats.head_switches == 0U);
    /* The shadow mirror is untouched (no transition, no sync mint). */
    TEST_ASSERT(recovery->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    TEST_ASSERT(recovery->cluster.transition_reason == UCN_CLUSTER_REASON_INIT);
    TEST_ASSERT(recovery->cluster.shadow_transition_count == 0U);
    TEST_ASSERT(test_derive_phase(&recovery->cluster, 60U) ==
                UCN_CLUSTER_PHASE_RECOVERY_HEAD);
    return 0;
}

/* CLV2-01-04f.2 (site A): the BACKUP missed-heartbeat eligible path in
 * ucn_cluster_step_inner().  The limit hit IS the BACKUP_SYNCING ->
 * RECOVERY_OBSERVE transition (PRIMARY_LOST - the primary's heartbeat
 * stream failed; NOT TAKEOVER_TIMEOUT - no takeover here) and must be
 * committed through the single entry point FIRST, UNCONDITIONALLY, fail
 * closed; apply_legacy(RECOVERY_OBSERVE) owns role=DETACHED +
 * eligible=true + backoff/grace/known_backup=0, and the Current-order
 * site writes (stats++ / redundant-but-harmless eligible=true /
 * idempotent backup_clear_sync()) stay after it.  Sub-cases:
 *  (A1) SYNCING + missed-heartbeat limit + expired primary deadline ->
 *       BACKUP_SYNCING -> RECOVERY_OBSERVE (PRIMARY_LOST), stats++,
 *       eligible=true, backup fields cleared, role DETACHED;
 *  (A2) shadow-desync sibling: a stale shadow (BACKUP_READY) while the
 *       legacy derives BACKUP_SYNCING rejects the transition fail-closed
 *       -> step returns UCN_ERR_STATE with ZERO phase-relevant writes
 *       (backup fields untouched, stats unchanged, eligible stays false,
 *       mirror not minted - ucn_cluster_step syncs only on UCN_OK, so
 *       the stale shadow stays untouched like the e.5 sibling); the next
 *       step re-visits the still-expired deadline and fails again;
 *  (A3) M01.0.2 preservation: (a) a takeover-active Backup (M01.0.2
 *       takeover && syncing combo) with missed heartbeats still takes the
 *       e.5 takeover-timeout path (BACKUP_TAKEOVER -> DETACHED_OBSERVE,
 *       TAKEOVER_TIMEOUT), never this eligible path; (b/c) the READY +
 *       !takeover precedence (L5538) is unchanged: READY + lease expired
 *       starts takeover (BACKUP_READY -> BACKUP_TAKEOVER, TAKEOVER_STARTED)
 *       and READY + lease NOT expired keeps waiting (stays BACKUP_READY). */
static int cluster_test_backup_miss_eligible_wiring(void)
{
    cluster_test_network_t network;
    ucn_cluster_t *c;
    ucn_result_t result;
    uint32_t before;
    uint32_t baseline;

    /* ============ (A1) eligible happy path: BACKUP_SYNCING -> RECOVERY_OBSERVE ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 1U) == 0);
    network.now_ms = 100U;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = 1U;
    c->backup_primary_node_id = 1U;
    c->backup_generation = 7U;
    c->backup_ready = false;
    c->backup_syncing = true;
    c->backup_takeover_active = false;
    c->backup_primary_deadline_ms = 10U; /* expired at now == 100 */
    c->backup_missed_heartbeats =
        UCN_CLUSTER_BACKUP_MISS_LIMIT - 1U; /* this step reaches the limit */
    c->known_backup_node_id = 1U;
    c->known_backup_generation = 7U;
    c->recovery_eligible = false;
    c->membership_sequence = 4U;
    (void)memset(c->primary_members.slots, 0, sizeof(c->primary_members.slots));
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = 2U;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_SYNCING;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    before = c->stats.head_leases_expired;
    TEST_ASSERT(ucn_cluster_step(c) == UCN_OK);
    /* explicit BACKUP_SYNCING -> RECOVERY_OBSERVE, PRIMARY_LOST, count+1. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_PRIMARY_LOST);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->stats.head_leases_expired == before + 1U);
    /* apply_legacy + site effects in original order. */
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(c->recovery_eligible == true);
    TEST_ASSERT(c->recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(c->head_grace_deadline_ms == 0U);
    TEST_ASSERT(c->known_backup_node_id == 0U);
    TEST_ASSERT(c->known_backup_generation == 0U);
    /* backup_clear_sync() site effects (idempotent cleanup). */
    TEST_ASSERT(c->backup_syncing == false);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->backup_primary_node_id == 0U);
    TEST_ASSERT(c->backup_generation == 0U);
    TEST_ASSERT(c->membership_sequence == 0U);
    TEST_ASSERT(c->backup_primary_deadline_ms == 0U);
    TEST_ASSERT(c->backup_missed_heartbeats == 0U);
    TEST_ASSERT(c->primary_members.slots[0].occupied == false);
    TEST_ASSERT(test_derive_phase(c, 100U) ==
                UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);

    /* ============ (A2) desync sibling: stale shadow -> UCN_ERR_STATE, zero phase-relevant writes ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 1U) == 0);
    network.now_ms = 100U;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = 1U;
    c->backup_primary_node_id = 1U;
    c->backup_generation = 7U;
    c->backup_ready = false;
    c->backup_syncing = true;
    c->backup_takeover_active = false;
    c->backup_primary_deadline_ms = 10U; /* expired */
    c->backup_missed_heartbeats =
        UCN_CLUSTER_BACKUP_MISS_LIMIT - 1U;
    c->known_backup_node_id = 1U;
    c->known_backup_generation = 7U;
    c->recovery_eligible = false;
    c->membership_sequence = 4U;
    (void)memset(c->primary_members.slots, 0, sizeof(c->primary_members.slots));
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = 2U;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY; /* stale */
    c->transition_reason = UCN_CLUSTER_REASON_INIT;
    c->shadow_transition_count = 0U;
    before = c->stats.head_leases_expired;
    ucn_cluster_test_transition_asserts_set(false);
    result = ucn_cluster_step(c);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(result == UCN_ERR_STATE);
    /* The deadline-expired counter increment runs BEFORE the transition
     * (Current order: it gates the limit check); it is not phase-relevant
     * and the fail-closed return never reaches the deadline re-arm. */
    TEST_ASSERT(c->backup_missed_heartbeats ==
                UCN_CLUSTER_BACKUP_MISS_LIMIT);
    /* ZERO phase-relevant writes: no stats++, no detach, mirror intact. */
    TEST_ASSERT(c->stats.head_leases_expired == before);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(c->recovery_eligible == false);
    TEST_ASSERT(c->backup_syncing == true);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->backup_takeover_active == false);
    TEST_ASSERT(c->backup_primary_node_id == 1U);
    TEST_ASSERT(c->backup_generation == 7U);
    TEST_ASSERT(c->membership_sequence == 4U);
    TEST_ASSERT(c->backup_primary_deadline_ms == 10U); /* not re-armed */
    TEST_ASSERT(c->known_backup_node_id == 1U);
    TEST_ASSERT(c->known_backup_generation == 7U);
    TEST_ASSERT(c->primary_members.slots[0].occupied == true);
    /* ucn_cluster_step syncs only on UCN_OK, so the stale shadow stays
     * untouched (e.5 sibling convention). */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_READY);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_INIT);
    TEST_ASSERT(c->shadow_transition_count == 0U);
    /* The next step re-visits the still-expired deadline: same fail-closed
     * outcome, still zero phase-relevant writes. */
    ucn_cluster_test_transition_asserts_set(false);
    result = ucn_cluster_step(c);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(result == UCN_ERR_STATE);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(c->recovery_eligible == false);
    TEST_ASSERT(c->stats.head_leases_expired == before);

    /* ============ (A3a) M01.0.2: takeover-active (&& syncing combo) still takes the e.5 timeout path ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 1U) == 0);
    network.now_ms = 100U;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = 1U;
    c->backup_primary_node_id = 1U;
    c->backup_generation = 7U;
    c->backup_ready = false;
    c->backup_syncing = true; /* M01.0.2 combo */
    c->backup_takeover_active = true;
    c->backup_takeover_deadline_ms = 10U; /* expired: e.5 path */
    c->backup_primary_deadline_ms = 10U;  /* expired */
    c->backup_missed_heartbeats = UCN_CLUSTER_BACKUP_MISS_LIMIT;
    c->recovery_eligible = false;
    c->membership_sequence = 4U;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) ==
                UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    before = c->stats.head_leases_expired;
    TEST_ASSERT(ucn_cluster_step(c) == UCN_OK);
    /* e.5 path (TAKEOVER_TIMEOUT), never the missed-heartbeat eligible
     * path (no RECOVERY_OBSERVE, no PRIMARY_LOST). */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_TAKEOVER_TIMEOUT);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->stats.head_leases_expired == before + 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(c->recovery_eligible == false);
    TEST_ASSERT(c->backup_syncing == false);
    /* backup_clear_sync() never clears takeover_active: the flag lingers
     * after detach (Current behavior - the e.5 test pins the same set). */
    TEST_ASSERT(c->backup_takeover_active == true);
    TEST_ASSERT(c->backup_primary_node_id == 0U);
    TEST_ASSERT(c->backup_generation == 0U);
    TEST_ASSERT(c->backup_missed_heartbeats == 0U);
    TEST_ASSERT(c->backup_primary_deadline_ms == 0U);
    TEST_ASSERT(test_derive_phase(c, 100U) ==
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE);

    /* ============ (A3b) READY + !takeover precedence: lease expired -> takeover ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 1U) == 0);
    network.now_ms = 100U;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = 1U;
    c->backup_primary_node_id = 1U;
    c->backup_generation = 7U;
    c->backup_ready = true;
    c->backup_syncing = false;
    c->backup_takeover_active = false;
    c->backup_primary_deadline_ms = 10U;            /* expired */
    c->backup_primary_lease_deadline_ms = 10U;      /* expired: §5.1 satisfied */
    c->backup_missed_heartbeats = UCN_CLUSTER_BACKUP_MISS_LIMIT;
    c->recovery_eligible = false;
    (void)memset(c->primary_members.slots, 0, sizeof(c->primary_members.slots));
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = c->config.local_node_id; /* self vote */
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) ==
                UCN_CLUSTER_PHASE_BACKUP_READY);
    baseline = c->shadow_transition_count;
    TEST_ASSERT(ucn_cluster_step(c) == UCN_OK);
    /* READY-takeover precedence unchanged: BACKUP_READY -> BACKUP_TAKEOVER,
     * never RECOVERY_OBSERVE. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_TAKEOVER_STARTED);
    TEST_ASSERT(c->shadow_transition_count == baseline + 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(c->backup_takeover_active == true);
    TEST_ASSERT(c->recovery_eligible == false);
    /* M01.0.2: start_takeover never clears ready/syncing. */
    TEST_ASSERT(c->backup_ready == true);
    TEST_ASSERT(c->backup_syncing == false);

    /* ============ (A3c) READY + !takeover + lease NOT expired -> keep waiting ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 1U) == 0);
    network.now_ms = 100U;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = 1U;
    c->backup_primary_node_id = 1U;
    c->backup_generation = 7U;
    c->backup_ready = true;
    c->backup_syncing = false;
    c->backup_takeover_active = false;
    c->backup_primary_deadline_ms = 10U;        /* expired */
    c->backup_primary_lease_deadline_ms = 500U; /* NOT expired */
    c->backup_missed_heartbeats = UCN_CLUSTER_BACKUP_MISS_LIMIT;
    c->recovery_eligible = false;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(ucn_cluster_step(c) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_READY);
    TEST_ASSERT(c->shadow_transition_count == 0U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(c->backup_takeover_active == false);
    TEST_ASSERT(c->recovery_eligible == false);
    TEST_ASSERT(c->backup_primary_deadline_ms ==
                ucn_deadline_from_now(
                    100U, c->config.keepalive_interval_ms)); /* re-armed */
    return 0;
}

/* CLV2-01-04f.2 (site B): the STEPPING_DOWN deadline in
 * ucn_cluster_step_inner().  The expired stepdown deadline IS the
 * STEPPING_DOWN -> JOIN_PENDING transition (STEPDOWN_COMPLETE - the
 * ordered switchback completed and the node rejoins the better Head) and
 * must be committed through the single entry point FIRST, UNCONDITIONALLY,
 * fail closed; apply_legacy(JOIN_PENDING) owns role=JOIN_PENDING +
 * eligible=false + backoff=0, and the Current-order site effects
 * (clear_members() + idempotent role write + role_since/next_join_retry/
 * stepdown_deadline timers) stay after it.  The pending Head identity is
 * preserved.  Sub-cases:
 *  (B1) deadline expired -> STEPPING_DOWN -> JOIN_PENDING (STEPDOWN_
 *       COMPLETE), members cleared, role JOIN_PENDING, timers updated,
 *       pending fields preserved;
 *  (B2) shadow-desync sibling: a stale shadow (RECOVERY_OBSERVE) while
 *       the legacy derives STEPPING_DOWN rejects the transition fail-
 *       closed -> step returns UCN_ERR_STATE with ZERO writes (members
 *       intact, role stays STEPPING_DOWN, deadline/timers/pending
 *       untouched, mirror not minted - no sync on error); the next step
 *       re-visits and fails again. */
static int cluster_test_stepdown_deadline_wiring(void)
{
    cluster_test_network_t network;
    ucn_cluster_t *c;
    ucn_result_t result;

    /* ============ (B1) happy path: STEPPING_DOWN -> JOIN_PENDING ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 1U) == 0);
    network.now_ms = 100U;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_STEPPING_DOWN;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = 1U;
    c->stepdown_deadline_ms = 10U; /* expired at now == 100 */
    c->role_since_ms = 90U;
    c->next_join_retry_ms = 500U;
    c->pending_head_node_id = 2U;
    c->pending_cluster_id = 1U;
    c->pending_term = 5U;
    c->pending_head_score = 9000U;
    c->recovery_eligible = true;         /* apply_legacy must clear it */
    c->recovery_backoff_deadline_ms = 77U; /* apply_legacy must clear it */
    (void)memset(c->primary_members.slots, 0, sizeof(c->primary_members.slots));
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = 2U;
    c->primary_members.slots[0].lease_expires_at_ms = 60U;
    c->shadow_phase = UCN_CLUSTER_PHASE_STEPPING_DOWN;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) ==
                UCN_CLUSTER_PHASE_STEPPING_DOWN);
    TEST_ASSERT(ucn_cluster_step(c) == UCN_OK);
    /* explicit STEPPING_DOWN -> JOIN_PENDING, STEPDOWN_COMPLETE, count+1. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_STEPDOWN_COMPLETE);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    /* apply_legacy(JOIN_PENDING) writes. */
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(c->recovery_eligible == false);
    TEST_ASSERT(c->recovery_backoff_deadline_ms == 0U);
    /* Current-order site effects. */
    TEST_ASSERT(c->primary_members.slots[0].occupied == false); /* clear_members */
    TEST_ASSERT(c->role_since_ms == 100U);
    /* The JOIN_PENDING block in the SAME step sees the freshly-armed
     * retry deadline (== now) and immediately sends the first join
     * request, which re-arms next_join_retry_ms (Current behavior). */
    TEST_ASSERT(c->next_join_retry_ms ==
                ucn_deadline_from_now(100U, c->config.join_retry_ms));
    TEST_ASSERT(c->stepdown_deadline_ms == 0U);
    /* pending Head identity preserved. */
    TEST_ASSERT(c->pending_head_node_id == 2U);
    TEST_ASSERT(c->pending_cluster_id == 1U);
    TEST_ASSERT(c->pending_term == 5U);
    TEST_ASSERT(c->pending_head_score == 9000U);
    TEST_ASSERT(test_derive_phase(c, 100U) ==
                UCN_CLUSTER_PHASE_JOIN_PENDING);

    /* ============ (B2) desync sibling: stale shadow -> UCN_ERR_STATE, zero writes ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 1U) == 0);
    network.now_ms = 100U;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_STEPPING_DOWN;
    c->cluster_id = 1U;
    c->term = 5U;
    c->head_node_id = 1U;
    c->stepdown_deadline_ms = 10U; /* expired */
    c->role_since_ms = 90U;
    c->next_join_retry_ms = 500U;
    c->pending_head_node_id = 2U;
    c->pending_cluster_id = 1U;
    c->pending_term = 5U;
    c->pending_head_score = 9000U;
    c->recovery_eligible = true;
    c->recovery_backoff_deadline_ms = 77U;
    (void)memset(c->primary_members.slots, 0, sizeof(c->primary_members.slots));
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = 2U;
    c->primary_members.slots[0].lease_expires_at_ms = 60U;
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_OBSERVE; /* stale */
    c->transition_reason = UCN_CLUSTER_REASON_INIT;
    c->shadow_transition_count = 0U;
    ucn_cluster_test_transition_asserts_set(false);
    result = ucn_cluster_step(c);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(result == UCN_ERR_STATE);
    /* ZERO writes: members intact, role stays STEPPING_DOWN, timers and
     * pending identity untouched, apply_legacy never ran. */
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_STEPPING_DOWN);
    TEST_ASSERT(c->primary_members.slots[0].occupied == true);
    TEST_ASSERT(c->primary_members.slots[0].node_id == 2U);
    TEST_ASSERT(c->primary_members.slots[0].lease_expires_at_ms == 60U);
    TEST_ASSERT(c->stepdown_deadline_ms == 10U); /* not disarmed */
    TEST_ASSERT(c->role_since_ms == 90U);
    TEST_ASSERT(c->next_join_retry_ms == 500U);
    TEST_ASSERT(c->pending_head_node_id == 2U);
    TEST_ASSERT(c->pending_cluster_id == 1U);
    TEST_ASSERT(c->pending_term == 5U);
    TEST_ASSERT(c->pending_head_score == 9000U);
    TEST_ASSERT(c->recovery_eligible == true); /* apply_legacy never ran */
    TEST_ASSERT(c->recovery_backoff_deadline_ms == 77U);
    /* ucn_cluster_step syncs only on UCN_OK, so the stale shadow stays
     * untouched (e.5 sibling convention). */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_INIT);
    TEST_ASSERT(c->shadow_transition_count == 0U);
    /* The next step re-visits the still-expired deadline: same fail-closed
     * outcome, still zero writes. */
    ucn_cluster_test_transition_asserts_set(false);
    result = ucn_cluster_step(c);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(result == UCN_ERR_STATE);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_STEPPING_DOWN);
    TEST_ASSERT(c->primary_members.slots[0].occupied == true);
    TEST_ASSERT(c->stepdown_deadline_ms == 10U);
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
    cluster_test_id_provider_t provider;
    size_t provider_index;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    /* Primary and Backup are both gone; only the head-capable node 3 and
     * member node 4 survive as a headless domain. */
    network.nodes[0].alive = false;
    network.nodes[1].alive = false;
    candidate = &network.nodes[2];
    peer = &network.nodes[3];
    (void)memset(&provider, 0, sizeof(provider));
    provider.ids[0] = UINT32_C(0x31415926);
    provider.ids[1] = UINT32_C(0x27182818);
    for (provider_index = 2U;
         provider_index < sizeof(provider.ids) / sizeof(provider.ids[0]);
         ++provider_index) {
        provider.ids[provider_index] =
            UINT32_C(0x40000000) + (uint32_t)provider_index;
    }
    provider.supplied = sizeof(provider.ids) / sizeof(provider.ids[0]);
    provider.result = UCN_OK;
    candidate->cluster.config.make_cluster_id = cluster_test_make_id;
    candidate->cluster.config.cluster_id_context = &provider;
    candidate->cluster.config.cluster_id_incarnation = UINT32_C(42);
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
    TEST_ASSERT(candidate->cluster.cluster_id == provider.ids[0]);
    TEST_ASSERT(candidate->cluster.recovery_cluster_id ==
                candidate->cluster.cluster_id);
    TEST_ASSERT(candidate->cluster.term == 1U);
    TEST_ASSERT(candidate->cluster.head_node_id == candidate->node_id);
    TEST_ASSERT(candidate->cluster.recovery_ack_count >= 1U);
    TEST_ASSERT(provider.count == 1U);
    TEST_ASSERT(provider.requests[0].purpose == UCN_CLUSTER_ID_PURPOSE_RECOVERY);
    TEST_ASSERT(provider.requests[0].parent_cluster_id == UINT32_C(1));
    TEST_ASSERT(provider.requests[0].parent_term == 1U);
    TEST_ASSERT(provider.requests[0].incarnation == UINT32_C(42));
    TEST_ASSERT(provider.requests[0].round == 1U);

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
    TEST_ASSERT(candidate->cluster.cluster_id != provider.ids[0]);
    TEST_ASSERT(provider.count >= 2U);
    TEST_ASSERT(candidate->cluster.cluster_id_round >= 2U);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_TAKEOVER;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 2U;
    message.head_node_id = peer->node_id;
    message.head_score = 9000U;
    message.lease_ms = 8000U;
    message.backup_generation = 1U;
    /* This focused Recovery fixture constructs the stable return proof
     * directly; the independent detach-history test covers its production
     * recording path. */
    candidate->cluster.last_cluster_id = 1U;
    candidate->cluster.max_seen_term = 1U;
    candidate->cluster.last_stable_head = UINT32_C(1);
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
 * The surviving headless member must switch to MEMBER of the provider-
 * allocated recovery Cluster (with declaring node as Head), and the
 * Recovery Head must track it as a member. */
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
    TEST_ASSERT(peer->cluster.cluster_id == candidate->cluster.cluster_id);
    TEST_ASSERT(peer->cluster.head_node_id == candidate->node_id);
    TEST_ASSERT(peer->cluster.term == 1U);
    /* The Recovery Head tracks the member. */
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (candidate->cluster.primary_members.slots[index].occupied &&
            candidate->cluster.primary_members.slots[index].node_id == peer->node_id) {
            found = true;
            break;
        }
    }
    TEST_ASSERT(found);
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        const ucn_cluster_member_t *member =
            &candidate->cluster.primary_members.slots[index];

        if (!member->occupied || member->node_id != peer->node_id) {
            continue;
        }
        TEST_ASSERT(ucn_cluster_member_record_is_valid(member));
        TEST_ASSERT(member->status == UCN_CLUSTER_MEMBER_STATUS_COMMITTED);
        TEST_ASSERT(member->voting);
        TEST_ASSERT(member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V3);
        TEST_ASSERT(member->capabilities == 0U);
        TEST_ASSERT(member->joined_at_ms <= now_ms);
        TEST_ASSERT(member->last_keepalive_at_ms <= now_ms);
        break;
    }
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

static int cluster_test_recovery_step_transitions(void)
{
    cluster_test_network_t network;
    ucn_cluster_t *c;
    const uint32_t now_ms = 100U;

    /* ---------- (a) arm backoff: RECOVERY_OBSERVE -> RECOVERY_ELECTION ---------- */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = now_ms;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_DETACHED;
    c->recovery_eligible = true;
    c->recovery_backoff_deadline_ms = 0U;
    c->recovery_cooldown_until_ms = 0U;
    c->observation_deadline_ms = 1U; /* expired */
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_OBSERVE;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    TEST_ASSERT(ucn_cluster_step(c) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_ELECTION);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_BACKOFF);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(c->recovery_eligible == true);
    /* The site still owns the armed backoff (nonce + deadline). */
    TEST_ASSERT(c->recovery_nonce != 0U);
    TEST_ASSERT(c->recovery_backoff_deadline_ms != 0U);
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_ELECTION);

    /* ---------- (b) declare: RECOVERY_ELECTION -> RECOVERY_HEAD ---------- */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = now_ms;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_DETACHED;
    c->recovery_eligible = true;
    c->recovery_backoff_deadline_ms = now_ms - 1U; /* expired */
    c->recovery_cooldown_until_ms = 0U;
    c->observation_deadline_ms = 1U;
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_ELECTION;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_ELECTION);
    TEST_ASSERT(ucn_cluster_step(c) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_HEAD);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_WIN);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    /* The site's declare payload stays site-owned in original order. */
    TEST_ASSERT(c->recovery_cluster_id != 0U);
    TEST_ASSERT(c->recovery_cluster_id != c->config.local_node_id);
    TEST_ASSERT(c->cluster_id == c->recovery_cluster_id);
    TEST_ASSERT(c->term == 1U);
    TEST_ASSERT(c->head_node_id == c->config.local_node_id);
    TEST_ASSERT(c->recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(c->recovery_deadline_ms != 0U);
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_HEAD);

    /* ---------- (c) TTL stepdown: RECOVERY_HEAD -> RECOVERY_OBSERVE ---------- */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = now_ms;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    c->recovery_eligible = true; /* stepdown keeps it (real-path invariant) */
    c->recovery_deadline_ms = 1U; /* expired */
    c->next_advertise_ms = now_ms + 1000U; /* skip the advertise branch */
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_HEAD);
    TEST_ASSERT(ucn_cluster_step(c) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    TEST_ASSERT(c->transition_reason ==
                UCN_CLUSTER_REASON_RECOVERY_TTL_EXPIRED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(c->recovery_eligible == true); /* kept by stepdown_recovery_head */
    /* stepdown_recovery_head() site effects in original order. */
    TEST_ASSERT(c->recovery_cooldown_until_ms != 0U); /* cooldown armed */
    TEST_ASSERT(c->recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(c->recovery_cluster_id == 0U);
    TEST_ASSERT(c->recovery_deadline_ms == 0U);
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);

    /* ---------- (d) non-quorum backoff re-arm: PHASE PRESERVED ---------- */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = now_ms;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_DETACHED;
    c->recovery_eligible = true;
    c->recovery_backoff_deadline_ms = now_ms - 1U; /* expired */
    c->recovery_cooldown_until_ms = 0U;
    c->observation_deadline_ms = 1U;
    (void)memset(c->peers, 0, sizeof(c->peers)); /* no visible quorum */
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_ELECTION;
    c->transition_reason = UCN_CLUSTER_REASON_RECOVERY_BACKOFF;
    c->shadow_transition_count = 5U;
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_ELECTION);
    TEST_ASSERT(ucn_cluster_step(c) == UCN_OK);
    /* No transition: shadow/reason/count unchanged, the re-arm is a
     * plain site write. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_ELECTION);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_BACKOFF);
    TEST_ASSERT(c->shadow_transition_count == 5U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(c->recovery_eligible == true);
    TEST_ASSERT(c->recovery_backoff_deadline_ms != now_ms - 1U); /* re-armed */
    TEST_ASSERT(c->recovery_backoff_deadline_ms != 0U);
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_ELECTION);

    /* ---------- (e1) desync: arm backoff, stale shadow ---------- */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = now_ms;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_DETACHED;
    c->recovery_eligible = true;
    c->recovery_backoff_deadline_ms = 0U;
    c->recovery_cooldown_until_ms = 0U;
    c->observation_deadline_ms = 1U;
    c->recovery_nonce = 0U;
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_ELECTION; /* stale */
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    ucn_cluster_test_transition_asserts_set(false);
    TEST_ASSERT(ucn_cluster_step(c) == UCN_ERR_STATE);
    ucn_cluster_test_transition_asserts_set(true);
    /* ZERO site writes: backoff NOT armed, shadow untouched (the
     * end-of-step sync only runs on UCN_OK - e.5 precedent). */
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(c->recovery_eligible == true);
    TEST_ASSERT(c->recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(c->recovery_nonce == 0U);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_ELECTION);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_UNKNOWN);
    TEST_ASSERT(c->shadow_transition_count == 0U);
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);

    /* ---------- (e2) desync: declare, stale shadow ---------- */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = now_ms;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_DETACHED;
    c->recovery_eligible = true;
    c->recovery_backoff_deadline_ms = now_ms - 1U; /* expired */
    c->recovery_cooldown_until_ms = 0U;
    c->observation_deadline_ms = 1U;
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_OBSERVE; /* stale */
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_ELECTION);
    ucn_cluster_test_transition_asserts_set(false);
    TEST_ASSERT(ucn_cluster_step(c) == UCN_ERR_STATE);
    ucn_cluster_test_transition_asserts_set(true);
    /* ZERO site writes: still DETACHED, nothing declared. */
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(c->recovery_cluster_id == 0U);
    TEST_ASSERT(c->cluster_id == 0U);
    TEST_ASSERT(c->term == 0U);
    TEST_ASSERT(c->head_node_id == 0U);
    TEST_ASSERT(c->recovery_backoff_deadline_ms == now_ms - 1U);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_UNKNOWN);
    TEST_ASSERT(c->shadow_transition_count == 0U);
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_ELECTION);

    /* ---------- (e3) desync: TTL stepdown, stale shadow ---------- */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = now_ms;
    c = &network.nodes[0].cluster;
    c->role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    c->recovery_eligible = true; /* real-path invariant */
    c->recovery_deadline_ms = 1U; /* expired */
    c->recovery_cooldown_until_ms = 0U;
    c->recovery_cluster_id = 7U;
    c->next_advertise_ms = now_ms + 1000U; /* skip the advertise branch */
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_OBSERVE; /* stale */
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_HEAD);
    ucn_cluster_test_transition_asserts_set(false);
    TEST_ASSERT(ucn_cluster_step(c) == UCN_ERR_STATE);
    ucn_cluster_test_transition_asserts_set(true);
    /* ZERO site writes: still RECOVERY_HEAD, cooldown NOT armed. */
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    TEST_ASSERT(c->recovery_cooldown_until_ms == 0U);
    TEST_ASSERT(c->recovery_cluster_id == 7U);
    TEST_ASSERT(c->recovery_deadline_ms == 1U);
    TEST_ASSERT(c->recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_UNKNOWN);
    TEST_ASSERT(c->shadow_transition_count == 0U);
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_HEAD);

    /* ---------- (f) CLV2-M12 (12-03): the zero-backoff spin is gone ----------
     * A node whose id is a multiple of recovery_backoff_max_ms used to
     * compute node_id % max == 0 and spin in RECOVERY_OBSERVE forever
     * (the M01 OP-210 documented deficiency, fixed by M12).  The validated
     * exponential backoff is always non-zero and bounded by max, so the
     * RECOVERY_OBSERVE -> RECOVERY_ELECTION transition now commits
     * normally: the armed backoff deadline is non-zero, the phase/reason/
     * count reflect it, and the site still owns the nonce + deadline
     * writes. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = now_ms;
    c = &network.nodes[0].cluster;
    c->config.local_node_id = 5U; /* used to compute backoff 0 pre-M12 */
    c->role = UCN_CLUSTER_ROLE_DETACHED;
    c->recovery_eligible = true;
    c->recovery_backoff_deadline_ms = 0U;
    c->recovery_cooldown_until_ms = 0U;
    c->observation_deadline_ms = 1U; /* expired */
    c->recovery_nonce = 0U;
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_OBSERVE;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    TEST_ASSERT(ucn_cluster_step(c) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_ELECTION);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_BACKOFF);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(c->recovery_eligible == true);
    TEST_ASSERT(c->recovery_nonce != 0U); /* site write preserved */
    TEST_ASSERT(c->recovery_backoff_deadline_ms != 0U); /* non-zero backoff */
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_ELECTION);
    return 0;
}

static void cluster_test_recovery_declare_build(ucn_cluster_message_t *message,
                                                ucn_node_id_t head,
                                                uint32_t cluster_id,
                                                uint32_t term,
                                                uint32_t nonce,
                                                uint32_t ttl_ms)
{
    (void)memset(message, 0, sizeof(*message));
    message->type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message->role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message->cluster_id = cluster_id;
    message->term = term;
    message->head_node_id = head;
    message->recovery_nonce = nonce;
    message->recovery_ttl_ms = ttl_ms;
}

static size_t cluster_test_recovery_ack_queued(cluster_test_network_t *network,
                                               ucn_node_id_t destination)
{
    size_t index;
    size_t count = 0U;

    for (index = 0U; index < network->queue_count; ++index) {
        if (network->queue[index].destination == destination &&
            network->queue[index].payload[1U] ==
                (uint8_t)UCN_CLUSTER_MSG_RECOVERY_ACK) {
            count++;
        }
    }
    return count;
}

static int cluster_test_recovery_declare_wiring(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_t *c;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    /* ============ (a) RECOVERY_HEAD yield to a smaller contender ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2]; /* node 3: the incumbent Recovery Head */
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    c->cluster_id = 3U;
    c->term = 1U;
    c->head_node_id = 3U;
    c->recovery_cluster_id = 3U;
    c->recovery_nonce = 100U;
    c->recovery_eligible = true;
    c->recovery_cooldown_until_ms = 0U;
    c->recovery_backoff_deadline_ms = 0U;
    c->accepted_recovery_nonce = 0U;
    c->known_recovery_source = 0U;
    c->known_backup_node_id = 2U;
    c->known_backup_generation = 1U;
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_RECOVERY_HEAD);
    cluster_test_recovery_declare_build(&message, 4U, 4U, 1U, 50U, 30U);
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, 4U, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    /* the explicit RECOVERY_HEAD -> MEMBER_ACTIVE transition committed
     * first (RECOVERY_YIELDED, count +1), apply_legacy wrote
     * role/grace/eligible. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_YIELDED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->head_grace_deadline_ms == 0U);
    TEST_ASSERT(c->recovery_eligible == false);
    /* stepdown_recovery_head() site-owned effects in original order. */
    TEST_ASSERT(c->recovery_cooldown_until_ms == ucn_deadline_from_now(
                    100U, c->config.recovery_observation_ms));
    TEST_ASSERT(c->known_backup_node_id == 0U);
    TEST_ASSERT(c->known_backup_generation == 0U);
    /* the join block populated the winner's cluster and the ACK went out. */
    TEST_ASSERT(c->accepted_recovery_nonce == 50U);
    TEST_ASSERT(c->known_recovery_source == 4U);
    TEST_ASSERT(c->recovery_cluster_id == 4U);
    TEST_ASSERT(c->cluster_id == 4U);
    TEST_ASSERT(c->term == 1U);
    TEST_ASSERT(c->head_node_id == 4U);
    TEST_ASSERT(c->role_since_ms == 100U);
    TEST_ASSERT(c->election_deadline_ms == 0U);
    TEST_ASSERT(c->head_lease_expires_at_ms == 130U);
    TEST_ASSERT(c->recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(cluster_test_recovery_ack_queued(&network, 4U) == 1U);
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);

    /* ============ (b) RECOVERY_OBSERVE plain join ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2]; /* node 3: headless recovery-eligible node */
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_DETACHED;
    c->recovery_eligible = true;
    c->recovery_backoff_deadline_ms = 0U;
    c->recovery_cooldown_until_ms = 0U;
    c->recovery_nonce = 0U; /* never started own backoff: always accepts */
    c->accepted_recovery_nonce = 0U;
    c->known_recovery_source = 0U;
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_OBSERVE;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    cluster_test_recovery_declare_build(&message, 4U, 4U, 1U, 50U, 30U);
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, 4U, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_WIN);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->head_grace_deadline_ms == 0U);
    TEST_ASSERT(c->recovery_eligible == false);
    TEST_ASSERT(c->recovery_cluster_id == 4U);
    TEST_ASSERT(c->cluster_id == 4U);
    TEST_ASSERT(c->term == 1U);
    TEST_ASSERT(c->head_node_id == 4U);
    TEST_ASSERT(c->current_head_score == c->config.head_score);
    TEST_ASSERT(c->role_since_ms == 100U);
    TEST_ASSERT(c->election_deadline_ms == 0U);
    TEST_ASSERT(c->head_lease_expires_at_ms == 130U);
    TEST_ASSERT(c->recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(cluster_test_recovery_ack_queued(&network, 4U) == 1U);
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);

    /* RECOVERY_ELECTION variant: an armed backoff still joins. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2];
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_DETACHED;
    c->recovery_eligible = true;
    c->recovery_backoff_deadline_ms = 105U;
    c->recovery_cooldown_until_ms = 0U;
    c->recovery_nonce = 0U;
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_ELECTION;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_RECOVERY_ELECTION);
    cluster_test_recovery_declare_build(&message, 4U, 4U, 1U, 50U, 30U);
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, 4U, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_WIN);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(c->head_node_id == 4U);
    TEST_ASSERT(cluster_test_recovery_ack_queued(&network, 4U) == 1U);
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);

    /* ============ (c) BACKUP source joins ============ */
    /* BACKUP_SYNCING source. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2];
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->backup_ready = false;
    c->backup_syncing = true;
    c->backup_takeover_active = false;
    c->backup_primary_node_id = 1U;
    c->backup_primary_lease_deadline_ms = 50U; /* headless */
    c->backup_generation = 7U;
    c->recovery_nonce = 0U;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_SYNCING;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    cluster_test_recovery_declare_build(&message, 4U, 4U, 1U, 50U, 30U);
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, 4U, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_WIN);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->head_node_id == 4U);
    TEST_ASSERT(c->head_lease_expires_at_ms == 130U);
    TEST_ASSERT(cluster_test_recovery_ack_queued(&network, 4U) == 1U);

    /* BACKUP_READY source. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2];
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->backup_ready = true;
    c->backup_syncing = false;
    c->backup_takeover_active = false;
    c->backup_primary_node_id = 1U;
    c->backup_primary_lease_deadline_ms = 50U; /* headless */
    c->backup_generation = 7U;
    c->recovery_nonce = 0U;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_BACKUP_READY);
    cluster_test_recovery_declare_build(&message, 4U, 4U, 1U, 50U, 30U);
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, 4U, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_WIN);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->head_node_id == 4U);
    TEST_ASSERT(cluster_test_recovery_ack_queued(&network, 4U) == 1U);

    /* BACKUP_TAKEOVER owns a stronger, already-fenced Stable recovery path.
     * A temporary Recovery Head cannot interrupt it or leave the object in
     * the old MEMBER + takeover_active split state. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2];
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->backup_ready = false;
    c->backup_syncing = true;
    c->backup_takeover_active = true;
    c->backup_primary_node_id = 1U;
    c->backup_generation = 7U;
    c->recovery_nonce = 0U;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_legacy_state_valid(c) == true);
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    cluster_test_recovery_declare_build(&message, 4U, 4U, 1U, 50U, 30U);
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, 4U, true, encoded,
                                    sizeof(encoded)) == UCN_ERR_ACCESS);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_UNKNOWN);
    TEST_ASSERT(c->shadow_transition_count == 0U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(c->head_node_id != 4U);
    TEST_ASSERT(c->backup_takeover_active == true);
    TEST_ASSERT(c->backup_syncing == true);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(cluster_test_recovery_ack_queued(&network, 4U) == 0U);
    TEST_ASSERT(test_derive_phase(c, 100U) ==
                UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);

    /* ============ (d) MEMBER source with expired lease: self ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2];
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_MEMBER;
    c->cluster_id = 1U;
    c->term = 1U;
    c->head_node_id = 1U;
    c->current_head_score = 9000U;
    c->recovery_cluster_id = 0U;
    c->head_lease_expires_at_ms = 5U; /* expired at now == 100 */
    c->head_grace_deadline_ms = 0U;
    c->recovery_nonce = 0U;
    c->shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    cluster_test_recovery_declare_build(&message, 4U, 4U, 2U, 50U, 30U);
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, 4U, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    /* self: derive was already MEMBER_ACTIVE, NO transition minted. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_UNKNOWN);
    TEST_ASSERT(c->shadow_transition_count == 0U);
    /* the join refresh still ran in original order. */
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->cluster_id == 4U);
    TEST_ASSERT(c->term == 2U);
    TEST_ASSERT(c->head_node_id == 4U);
    TEST_ASSERT(c->head_lease_expires_at_ms == 130U);
    TEST_ASSERT(c->recovery_eligible == false);
    TEST_ASSERT(cluster_test_recovery_ack_queued(&network, 4U) == 1U);
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);

    /* ============ (e) re-declaration lease refresh: phase-preserving ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2];
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_MEMBER;
    c->cluster_id = 4U;
    c->term = 1U;
    c->head_node_id = 4U;
    c->recovery_cluster_id = 4U;
    c->head_lease_expires_at_ms = 100U;
    c->head_grace_deadline_ms = 0U;
    c->accepted_recovery_nonce = 50U;
    c->known_recovery_source = 4U;
    c->shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    cluster_test_recovery_declare_build(&message, 4U, 4U, 1U, 50U, 30U);
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, 4U, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_UNKNOWN);
    TEST_ASSERT(c->shadow_transition_count == 0U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->head_lease_expires_at_ms == 130U);
    TEST_ASSERT(c->head_grace_deadline_ms == 0U);
    /* Every exact refresh retries the idempotent ACK so an initially
     * backpressured or dropped ACK cannot strand the member. */
    TEST_ASSERT(cluster_test_recovery_ack_queued(&network, 4U) == 1U);

    /* ============ (f) shadow-desync siblings: fail closed, ZERO site writes ============ */
    /* (f1) RECOVERY_HEAD yield desync: stale shadow while the legacy
     * derives RECOVERY_HEAD -> UCN_ERR_STATE; the node stays
     * RECOVERY_HEAD (a later smaller contender may still win) and the
     * end-of-RX sync re-aligns the mirror to the unchanged phase. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2];
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    c->cluster_id = 3U;
    c->term = 1U;
    c->head_node_id = 3U;
    c->recovery_cluster_id = 3U;
    c->recovery_nonce = 100U;
    c->recovery_eligible = true;
    c->recovery_cooldown_until_ms = 0U;
    c->accepted_recovery_nonce = 0U;
    c->known_recovery_source = 0U;
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_ELECTION; /* stale */
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    cluster_test_recovery_declare_build(&message, 4U, 4U, 1U, 50U, 30U);
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    ucn_cluster_test_transition_asserts_set(false);
    TEST_ASSERT(ucn_cluster_receive(c, 4U, true, encoded,
                                    sizeof(encoded)) == UCN_ERR_STATE);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    TEST_ASSERT(c->cluster_id == 3U);
    TEST_ASSERT(c->term == 1U);
    TEST_ASSERT(c->head_node_id == 3U);
    TEST_ASSERT(c->recovery_cooldown_until_ms == 0U); /* stepdown never ran */
    TEST_ASSERT(c->accepted_recovery_nonce == 0U);
    TEST_ASSERT(c->known_recovery_source == 0U);
    TEST_ASSERT(c->recovery_eligible == true);
    TEST_ASSERT(network.queue_count == 0U); /* no ACK */
    /* the end-of-RX sync re-aligned the mirror to the unchanged phase. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_HEAD);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_WIN);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    /* the still-RECOVERY_HEAD node can still yield to a later contender. */
    TEST_ASSERT(ucn_cluster_receive(c, 4U, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_YIELDED);
    TEST_ASSERT(c->shadow_transition_count == 2U);

    /* (f2) RECOVERY_OBSERVE join desync: stale shadow while the legacy
     * derives RECOVERY_OBSERVE -> UCN_ERR_STATE, ZERO site writes; the
     * end-of-RX sync re-aligns to RECOVERY_OBSERVE. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2];
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_DETACHED;
    c->recovery_eligible = true;
    c->recovery_backoff_deadline_ms = 0U;
    c->recovery_cooldown_until_ms = 0U;
    c->recovery_nonce = 0U;
    c->accepted_recovery_nonce = 0U;
    c->known_recovery_source = 0U;
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_ELECTION; /* stale */
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    cluster_test_recovery_declare_build(&message, 4U, 4U, 1U, 50U, 30U);
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    ucn_cluster_test_transition_asserts_set(false);
    TEST_ASSERT(ucn_cluster_receive(c, 4U, true, encoded,
                                    sizeof(encoded)) == UCN_ERR_STATE);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(c->recovery_eligible == true);
    TEST_ASSERT(c->recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(c->accepted_recovery_nonce == 0U);
    TEST_ASSERT(c->known_recovery_source == 0U);
    TEST_ASSERT(c->cluster_id == 0U);
    TEST_ASSERT(c->head_node_id == 0U);
    TEST_ASSERT(c->head_lease_expires_at_ms == 0U);
    TEST_ASSERT(network.queue_count == 0U);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    TEST_ASSERT(c->shadow_transition_count == 1U);

    /* (f3) BACKUP_SYNCING join desync: stale shadow while the legacy
     * derives BACKUP_SYNCING -> UCN_ERR_STATE, mirror untouched; the
     * end-of-RX sync re-aligns to BACKUP_SYNCING. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2];
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->backup_ready = false;
    c->backup_syncing = true;
    c->backup_takeover_active = false;
    c->backup_primary_node_id = 1U;
    c->backup_primary_lease_deadline_ms = 50U; /* headless */
    c->backup_generation = 7U;
    c->recovery_nonce = 0U;
    c->accepted_recovery_nonce = 0U;
    c->known_recovery_source = 0U;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY; /* stale */
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    cluster_test_recovery_declare_build(&message, 4U, 4U, 1U, 50U, 30U);
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    ucn_cluster_test_transition_asserts_set(false);
    TEST_ASSERT(ucn_cluster_receive(c, 4U, true, encoded,
                                    sizeof(encoded)) == UCN_ERR_STATE);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->backup_syncing == true);
    TEST_ASSERT(c->backup_takeover_active == false);
    TEST_ASSERT(c->backup_primary_node_id == 1U);
    TEST_ASSERT(c->backup_generation == 7U);
    TEST_ASSERT(c->accepted_recovery_nonce == 0U);
    TEST_ASSERT(c->known_recovery_source == 0U);
    TEST_ASSERT(network.queue_count == 0U);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(c->shadow_transition_count == 1U);

    /* ============ (g) handle_recovery_ack: phase-preserving (audit) ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2]; /* node 3: the Recovery Head */
    c = &node->cluster;
    c->role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    c->cluster_id = 3U;
    c->term = 1U;
    c->head_node_id = 3U;
    c->recovery_cluster_id = 3U;
    c->recovery_nonce = 50U;
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    c->transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    c->shadow_transition_count = 5U; /* nonzero baseline */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_ACK;
    message.role = UCN_CLUSTER_ROLE_MEMBER; /* matches the production ACK */
    message.cluster_id = 3U;
    message.term = 1U;
    message.head_node_id = 3U;
    message.recovery_nonce = 50U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, 4U, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_HEAD);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_WIN);
    TEST_ASSERT(c->shadow_transition_count == 5U); /* no transition */
    /* the acknowledged member was tracked (C07.7 P0-1). */
    {
        size_t index;
        bool found = false;

        for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
            if (c->primary_members.slots[index].occupied &&
                c->primary_members.slots[index].node_id == 4U) {
                found = true;
                break;
            }
        }
        TEST_ASSERT(found);
    }
    return 0;
}

static int cluster_test_recovery_head_takeover_wiring(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_t *c;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    uint32_t source;

    /* ============ (a) RECOVERY_HEAD defers to a higher-Term Head ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2]; /* node 3: the incumbent Recovery Head */
    c = &node->cluster;
    source = network.nodes[0].node_id; /* the stable new Head (node 1) */
    c->role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    c->cluster_id = 3U;
    c->term = 1U;
    c->head_node_id = 3U;
    c->recovery_cluster_id = 3U;
    c->recovery_eligible = true;
    c->recovery_deadline_ms = 130U;
    c->known_backup_node_id = source;
    c->known_backup_generation = 1U;
    /* Recovery's active Cluster is fresh.  Only this remembered stable
     * history domain may be compared with a stable Backup takeover. */
    c->last_cluster_id = 1U;
    c->max_seen_term = 1U;
    c->last_stable_head = network.nodes[1].node_id;
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_RECOVERY_HEAD);

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_TAKEOVER;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 2U;
    message.term = 99U;
    message.head_node_id = source;
    message.head_score = 9000U;
    message.lease_ms = 40U;
    message.backup_generation = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    /* A foreign identity remains incomparable even with a numerically much
     * larger Term. */
    TEST_ASSERT(ucn_cluster_receive(c, source, true, encoded,
                                    sizeof(encoded)) == UCN_ERR_ACCESS);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    message.cluster_id = 1U;
    message.term = 1U; /* stale inside the remembered stable domain */
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, source, true, encoded,
                                    sizeof(encoded)) == UCN_ERR_REPLAY);
    message.term = 2U; /* newer than the remembered stable Term, not recovery */
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, source, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    /* the explicit RECOVERY_HEAD -> MEMBER_ACTIVE transition committed
     * first (RECOVERY_YIELDED, count +1); apply_legacy wrote
     * role/grace/eligible; the site clears + epoch refresh ran after. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_YIELDED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->head_grace_deadline_ms == 0U);
    /* recovery fields cleared (site-owned, original order). */
    TEST_ASSERT(c->recovery_eligible == false);
    TEST_ASSERT(c->recovery_cluster_id == 0U);
    TEST_ASSERT(c->recovery_deadline_ms == 0U);
    /* epoch refreshed from the message. */
    TEST_ASSERT(c->cluster_id == 1U);
    TEST_ASSERT(c->term == 2U);
    TEST_ASSERT(c->head_node_id == source);
    TEST_ASSERT(c->current_head_score == 9000U);
    TEST_ASSERT(c->head_lease_expires_at_ms == ucn_deadline_from_now(
                    100U, 40U));
    TEST_ASSERT(c->next_keepalive_ms == 100U);
    /* known_backup + pending clears stayed at the site. */
    TEST_ASSERT(c->known_backup_node_id == 0U);
    TEST_ASSERT(c->known_backup_generation == 0U);
    TEST_ASSERT(c->pending_head_node_id == 0U);
    TEST_ASSERT(c->pending_cluster_id == 0U);
    TEST_ASSERT(c->pending_term == 0U);
    TEST_ASSERT(c->backup_takeover_active == false);
    TEST_ASSERT(c->backup_syncing == false);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);

    /* ============ (b) shadow-desync sibling: fail closed, ZERO writes ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2];
    c = &node->cluster;
    source = network.nodes[0].node_id;
    c->role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    c->cluster_id = 3U;
    c->term = 1U;
    c->head_node_id = 3U;
    c->recovery_cluster_id = 3U;
    c->recovery_eligible = true;
    c->recovery_deadline_ms = 130U;
    c->known_backup_node_id = source;
    c->known_backup_generation = 1U;
    c->last_cluster_id = 1U;
    c->max_seen_term = 1U;
    c->last_stable_head = network.nodes[1].node_id;
    c->shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_ELECTION; /* stale */
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_RECOVERY_HEAD);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_TAKEOVER;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 2U;
    message.head_node_id = source;
    message.head_score = 9000U;
    message.lease_ms = 40U;
    message.backup_generation = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    ucn_cluster_test_transition_asserts_set(false);
    TEST_ASSERT(ucn_cluster_receive(c, source, true, encoded,
                                    sizeof(encoded)) == UCN_ERR_STATE);
    ucn_cluster_test_transition_asserts_set(true);
    /* NOTHING touched: role + recovery state kept, epoch NOT refreshed. */
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    TEST_ASSERT(c->recovery_eligible == true);
    TEST_ASSERT(c->recovery_cluster_id == 3U);
    TEST_ASSERT(c->recovery_deadline_ms == 130U);
    TEST_ASSERT(c->cluster_id == 3U);
    TEST_ASSERT(c->term == 1U);
    TEST_ASSERT(c->head_node_id == 3U);
    TEST_ASSERT(c->head_grace_deadline_ms == 0U);
    TEST_ASSERT(c->known_backup_node_id == source);
    TEST_ASSERT(c->known_backup_generation == 1U);
    /* the end-of-RX sync re-aligned the mirror to the unchanged phase. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_HEAD);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_WIN);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    /* a later well-formed HEAD_TAKEOVER is still accepted. */
    TEST_ASSERT(ucn_cluster_receive(c, source, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RECOVERY_YIELDED);
    TEST_ASSERT(c->shadow_transition_count == 2U);
    TEST_ASSERT(c->recovery_cluster_id == 0U);
    TEST_ASSERT(c->recovery_deadline_ms == 0U);
    TEST_ASSERT(c->cluster_id == 1U);
    TEST_ASSERT(c->term == 2U);
    TEST_ASSERT(c->head_node_id == source);

    /* ============ (c) M01.0.2 BACKUP_TAKEOVER (takeover_active && syncing) ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[1];
    c = &node->cluster;
    source = network.nodes[0].node_id;
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->cluster_id = 1U;
    c->term = 1U;
    c->head_node_id = source;
    c->backup_primary_node_id = source;
    c->backup_generation = 1U;
    c->backup_takeover_active = true;
    c->backup_syncing = true; /* the M01.0.2 reachable combo */
    c->backup_ready = false;
    c->known_backup_node_id = source;
    c->known_backup_generation = 1U;
    c->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_TAKEOVER;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 2U;
    message.head_node_id = source;
    message.head_score = 9000U;
    message.lease_ms = 40U;
    message.backup_generation = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, source, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_TAKEOVER_STARTED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->backup_takeover_active == false);
    TEST_ASSERT(c->backup_syncing == false);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->term == 2U);
    TEST_ASSERT(c->head_node_id == source);
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);

    /* ============ (d) JOIN_PENDING inbound (review A MINOR 1, f.6) ============
     * A join-pending node switched by the higher-Term HEAD_TAKEOVER commits
     * JOIN_PENDING -> MEMBER_ACTIVE (TAKEOVER_STARTED) through the entry
     * point FIRST - the second production site for the JOIN_PENDING ->
     * MEMBER_ACTIVE DIRECT edge (the 01-04b join-accept site is the other);
     * the end-of-RX sync must no longer mint it. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2];
    c = &node->cluster;
    source = network.nodes[0].node_id;
    c->role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    c->cluster_id = 1U; /* the takeover guard requires cluster_id match
                            for non-RECOVERY_HEAD roles */
    c->term = 1U;
    c->head_node_id = 0U;
    c->pending_head_node_id = 2U;
    c->pending_cluster_id = 2U;
    c->pending_term = 1U;
    c->known_backup_node_id = source;
    c->known_backup_generation = 1U;
    c->shadow_phase = UCN_CLUSTER_PHASE_JOIN_PENDING;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_JOIN_PENDING);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_TAKEOVER;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 2U; /* strictly higher */
    message.head_node_id = source;
    message.head_score = 9000U;
    message.lease_ms = 40U;
    message.backup_generation = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(c, source, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    /* the explicit JOIN_PENDING -> MEMBER_ACTIVE transition committed
     * first (TAKEOVER_STARTED, count +1); apply_legacy wrote
     * role/grace/eligible; the epoch refresh + pending/known_backup clears
     * ran at the site in original order. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_TAKEOVER_STARTED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->head_grace_deadline_ms == 0U);
    TEST_ASSERT(c->cluster_id == 1U);
    TEST_ASSERT(c->term == 2U);
    TEST_ASSERT(c->head_node_id == source);
    TEST_ASSERT(c->current_head_score == 9000U);
    TEST_ASSERT(c->head_lease_expires_at_ms == ucn_deadline_from_now(
                    100U, 40U));
    TEST_ASSERT(c->pending_head_node_id == 0U);
    TEST_ASSERT(c->pending_cluster_id == 0U);
    TEST_ASSERT(c->pending_term == 0U);
    TEST_ASSERT(c->known_backup_node_id == 0U);
    TEST_ASSERT(c->known_backup_generation == 0U);
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);

    /* ============ (e) JOIN_PENDING desync sibling: fail closed ============ */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2];
    c = &node->cluster;
    source = network.nodes[0].node_id;
    c->role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    c->cluster_id = 1U; /* guard requires cluster_id match */
    c->term = 1U;
    c->head_node_id = 0U;
    c->pending_head_node_id = 2U;
    c->pending_cluster_id = 2U;
    c->pending_term = 1U;
    c->known_backup_node_id = source;
    c->known_backup_generation = 1U;
    c->shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE; /* stale */
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(c, 100U) == UCN_CLUSTER_PHASE_JOIN_PENDING);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_TAKEOVER;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 2U;
    message.head_node_id = source;
    message.head_score = 9000U;
    message.lease_ms = 40U;
    message.backup_generation = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    ucn_cluster_test_transition_asserts_set(false);
    TEST_ASSERT(ucn_cluster_receive(c, source, true, encoded,
                                    sizeof(encoded)) == UCN_ERR_STATE);
    ucn_cluster_test_transition_asserts_set(true);
    /* NOTHING touched: still JOIN_PENDING, pending + known_backup kept,
     * epoch NOT refreshed; the end-of-RX sync re-aligned the mirror to
     * the unchanged JOIN_PENDING phase. */
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(c->pending_head_node_id == 2U);
    TEST_ASSERT(c->pending_cluster_id == 2U);
    TEST_ASSERT(c->pending_term == 1U);
    TEST_ASSERT(c->known_backup_node_id == source);
    TEST_ASSERT(c->known_backup_generation == 1U);
    TEST_ASSERT(c->cluster_id == 1U);
    TEST_ASSERT(c->term == 1U);
    TEST_ASSERT(c->head_node_id == 0U);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_JOIN_INITIATED);
    TEST_ASSERT(c->shadow_transition_count == 1U); /* the re-align mint */
    return 0;
}


static int cluster_test_stable_switchback(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    uint32_t nonce = 0U;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];
    /* node 0 holds A/1.  A competing node 1 Head identity for A/1 is a
     * safety conflict, not a better-score switchback candidate. */
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.config.head_score = 6000U;
    head->cluster.current_head_score = 6000U;
    head->cluster.role_since_ms = 0U;
    head->cluster.stepdown_deadline_ms = 0U;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    head->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    head->cluster.shadow_transition_count = 0U;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = network.nodes[1].node_id;
    message.head_score = 9500U;
    message.available_capacity = 3U;
    message.lease_ms = 8000U;

    network.now_ms = 60U;
    message.nonce = ++nonce;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster,
                                    network.nodes[1].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_TERM_CONFLICT);
    TEST_ASSERT(head->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT);
    TEST_ASSERT(head->cluster.transition_reason ==
                UCN_CLUSTER_REASON_TERM_CONFLICT);
    TEST_ASSERT(head->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(head->cluster.stats.head_switches == 0U);

    /* Same-Term repeats, regardless of score, cannot re-enable control. */
    message.head_score = 10000U;
    message.nonce = ++nonce;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster,
                                    network.nodes[1].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(ucn_cluster_step(&head->cluster) == UCN_OK);
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_TERM_CONFLICT);
    TEST_ASSERT(head->cluster.shadow_transition_count == 1U);

    /* The first permitted Current-FSM exit is strictly higher authority. */
    message.term = 2U;
    message.nonce = ++nonce;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster,
                                    network.nodes[1].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(head->cluster.pending_head_node_id ==
                network.nodes[1].node_id);
    TEST_ASSERT(head->cluster.pending_term == 2U);
    TEST_ASSERT(head->cluster.transition_reason ==
                UCN_CLUSTER_REASON_HIGHER_AUTHORITY);
    return 0;
}

/* CLV2-M03 (03-03): the HEAD branch of consider_head_offer() classifies
 * offers by Epoch relation instead of raw Term.  Four pinned groups
 * (human auditor):
 *   (1) foreign high-term  (local A/2 vs remote B/100)  -> FOREIGN,
 *       NEVER HIGHER: no surrender even though 100 > 2 AND the score is
 *       better - the exact "绝不能进入 HIGHER-authority 让位逻辑" case.
 *   (2) foreign low-term   (local A/100 vs remote B/2)  -> FOREIGN, same
 *       contract: cluster_id truncates the domain BEFORE any Term read.
 *   (3) same-cluster higher (local A/2 vs remote A/3)   -> LOWER ->
 *       (compare(local, remote): remote A/3 IS the higher authority)
 *       -> unified same-cluster authority path -> surrender (legitimate).
 *   (4) same-cluster same-term different Head           -> CONFLICT,
 *       enters the TERM_CONFLICT safe wait; score and Node ID cannot
 *       choose a winner. */
/* CLV2-03-R10: JOIN_PENDING re-target is a separate Epoch decision from
 * active Authority.  Exercise the public RX path so foreign Term values
 * cannot select different outcomes for the same foreign Cluster policy. */
static int cluster_test_join_pending_epoch_domain_retarget(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_node_id_t source;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    node = &network.nodes[0];
    source = network.nodes[1].node_id;
    network.now_ms = 100U;
    node->cluster.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    node->cluster.cluster_id = 0U;
    node->cluster.term = 0U;
    node->cluster.head_node_id = 0U;
    node->cluster.pending_cluster_id = 1U;
    node->cluster.pending_term = 100U;
    node->cluster.pending_head_node_id = source;
    node->cluster.pending_head_score = 100U;
    node->cluster.next_join_retry_ms = 77U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_JOIN_PENDING;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_JOIN_INITIATED;
    node->cluster.shadow_transition_count = 0U;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 2U;
    message.term = 1U;
    message.head_node_id = source;
    message.head_score = 9000U;
    message.available_capacity = 4U;
    message.lease_ms = 8000U;
    message.nonce = 1U;

    /* A/100 -> B/1 is foreign and re-targets without comparing 1 to 100. */
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, source, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING &&
                node->cluster.pending_cluster_id == 2U &&
                node->cluster.pending_term == 1U &&
                node->cluster.pending_head_node_id == source &&
                node->cluster.pending_head_score == 9000U &&
                node->cluster.next_join_retry_ms == network.now_ms);

    /* The opposite numeric ordering has the identical foreign policy. */
    node->cluster.pending_cluster_id = 1U;
    node->cluster.pending_term = 1U;
    node->cluster.pending_head_node_id = source;
    node->cluster.pending_head_score = 101U;
    node->cluster.next_join_retry_ms = 78U;
    message.term = 100U;
    message.nonce = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, source, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.pending_cluster_id == 2U &&
                node->cluster.pending_term == 100U &&
                node->cluster.pending_head_score == 9000U &&
                node->cluster.next_join_retry_ms == network.now_ms);

    /* Same Cluster only accepts a remote newer Term. */
    node->cluster.pending_cluster_id = 1U;
    node->cluster.pending_term = 2U;
    node->cluster.pending_head_node_id = source;
    node->cluster.pending_head_score = 102U;
    node->cluster.next_join_retry_ms = 79U;
    message.cluster_id = 1U;
    message.term = 3U;
    message.nonce = 3U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, source, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.pending_cluster_id == 1U &&
                node->cluster.pending_term == 3U &&
                node->cluster.pending_head_score == 9000U &&
                node->cluster.next_join_retry_ms == network.now_ms);

    /* A stale same-Cluster offer remains entirely outside retarget policy. */
    node->cluster.pending_head_score = 103U;
    node->cluster.next_join_retry_ms = 80U;
    message.term = 2U;
    message.head_score = 8000U;
    message.nonce = 4U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, source, true, encoded,
                                    sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.pending_cluster_id == 1U &&
                node->cluster.pending_term == 3U &&
                node->cluster.pending_head_node_id == source &&
                node->cluster.pending_head_score == 103U &&
                node->cluster.next_join_retry_ms == 80U &&
                node->cluster.shadow_phase == UCN_CLUSTER_PHASE_JOIN_PENDING &&
                node->cluster.shadow_transition_count == 0U);
    return 0;
}

static int cluster_test_epoch_classified_head_offer(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    ucn_cluster_candidate_t candidate;
    uint32_t now_ms = 60U; /* past head_min_tenure_ms = 50 */
    ucn_result_t rc;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];

    /* Common HEAD setup: Cluster A (1), term 2, Head = node0, tenure
     * elapsed, low local score so a better offer would win on score if
     * the score path were (wrongly) entered. */
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 2U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.config.head_score = 6000U;
    head->cluster.current_head_score = 6000U;
    head->cluster.role_since_ms = 0U;
    head->cluster.stepdown_deadline_ms = 0U;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    head->cluster.transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    head->cluster.shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(&head->cluster, now_ms) ==
                UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);

    /* Group 1: local A/2 vs remote B/100 (foreign high-term).  The
     * pre-03-03 code surrendered here (candidate->term > cluster->term
     * with NO cluster_id check).  03-03: FOREIGN -> no surrender, no
     * stepdown, no transition, no score bookkeeping.  rc == UCN_ERR_STATE
     * is the hook's "no transition happened" signal. */
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.occupied = true;
    candidate.head_node_id = network.nodes[1].node_id;
    candidate.cluster_id = 2U;   /* foreign Cluster B */
    candidate.term = 100U;       /* higher NUMBER, but NOT comparable */
    candidate.head_score = 9500U; /* better score - must not matter */
    candidate.better_samples = 200U; /* would cross the threshold if scored */
    rc = ucn_cluster_test_consider_head_offer(&head->cluster, &candidate,
                                              now_ms);
    TEST_ASSERT(rc == UCN_ERR_STATE); /* no transition committed */
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_HEAD);
    TEST_ASSERT(head->cluster.shadow_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    TEST_ASSERT(head->cluster.shadow_transition_count == 0U);
    TEST_ASSERT(head->cluster.stats.head_switches == 0U);
    TEST_ASSERT(head->cluster.pending_head_node_id == 0U);
    TEST_ASSERT(candidate.better_samples == 200U); /* foreign path is a no-op */

    /* Group 2: local A/100 vs remote B/2 (foreign low-term).  Same
     * FOREIGN contract in the reverse direction: the local Term is not
     * authority over B either, and B's Term is never read for authority. */
    head->cluster.term = 100U;
    candidate.cluster_id = 2U;   /* foreign Cluster B */
    candidate.term = 2U;         /* lower NUMBER, but NOT comparable */
    candidate.head_score = 9500U;
    candidate.better_samples = 200U;
    rc = ucn_cluster_test_consider_head_offer(&head->cluster, &candidate,
                                              now_ms);
    TEST_ASSERT(rc == UCN_ERR_STATE);
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_HEAD);
    TEST_ASSERT(head->cluster.shadow_transition_count == 0U);
    TEST_ASSERT(head->cluster.stats.head_switches == 0U);
    TEST_ASSERT(candidate.better_samples == 200U);

    /* Group 3: local A/2 vs remote A/3 (same-cluster higher Term).
     * relation == LOWER (compare(local, remote) returns LOWER because
     * local term 2 < remote term 3), so remote_is_higher_authority ==
     * true - this IS legitimate higher authority: the unified
     * same-cluster authority path -> ordered stepdown (exactly what
     * Group 1 must NOT do).  Never write this as "relation == HIGHER":
     * HIGHER would mean the REMOTE is stale. */
    head->cluster.term = 2U;
    candidate.cluster_id = 1U;   /* same Cluster A */
    candidate.term = 3U;
    candidate.head_node_id = network.nodes[1].node_id;
    candidate.head_score = 9500U;
    candidate.better_samples = 0U;
    rc = ucn_cluster_test_consider_head_offer(&head->cluster, &candidate,
                                              now_ms);
    TEST_ASSERT(rc == UCN_OK); /* transition committed (HIGHER_AUTHORITY) */
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_STEPPING_DOWN);
    TEST_ASSERT(head->cluster.shadow_phase == UCN_CLUSTER_PHASE_STEPPING_DOWN);
    TEST_ASSERT(head->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(head->cluster.stats.head_switches == 1U);
    TEST_ASSERT(head->cluster.pending_head_node_id ==
                network.nodes[1].node_id);
    TEST_ASSERT(head->cluster.pending_cluster_id == 1U);
    TEST_ASSERT(head->cluster.pending_term == 3U);
    TEST_ASSERT(head->cluster.transition_reason ==
                UCN_CLUSTER_REASON_HIGHER_AUTHORITY);

    /* Group 4: local A/2/head0 vs remote A/2/head1.  The same-Term conflict
     * is terminal for score/Node-ID logic; only a later higher Term exits
     * the wait. */
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 2U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.last_cluster_id = 1U;
    head->cluster.max_seen_term = 2U;
    head->cluster.last_stable_head = head->node_id;
    head->cluster.role_since_ms = 0U;
    head->cluster.stepdown_deadline_ms = 0U;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    head->cluster.transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    head->cluster.shadow_transition_count = 0U;
    head->cluster.stats.head_switches = 0U;

    candidate.cluster_id = 1U;   /* same Cluster A */
    candidate.term = 2U;         /* same Term -> CONFLICT */
    candidate.head_node_id = network.nodes[1].node_id;
    candidate.head_score = 6200U;
    candidate.better_samples = 7U;
    rc = ucn_cluster_test_consider_head_offer(&head->cluster, &candidate,
                                              now_ms);
    TEST_ASSERT(rc == UCN_OK);
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_TERM_CONFLICT);
    TEST_ASSERT(head->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT);
    TEST_ASSERT(head->cluster.transition_reason ==
                UCN_CLUSTER_REASON_TERM_CONFLICT);
    TEST_ASSERT(head->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(head->cluster.stats.head_switches == 0U);
    TEST_ASSERT(candidate.better_samples == 7U);

    candidate.head_score = 9500U;
    candidate.better_samples = 2U;
    rc = ucn_cluster_test_consider_head_offer(&head->cluster, &candidate,
                                              now_ms);
    TEST_ASSERT(rc == UCN_ERR_STATE); /* idempotent safe wait */
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_TERM_CONFLICT);
    TEST_ASSERT(head->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(head->cluster.stats.head_switches == 0U);
    TEST_ASSERT(candidate.better_samples == 2U);
    candidate.term = 3U;
    rc = ucn_cluster_test_consider_head_offer(&head->cluster, &candidate,
                                              now_ms);
    TEST_ASSERT(rc == UCN_OK);
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(head->cluster.pending_head_node_id == candidate.head_node_id);
    TEST_ASSERT(head->cluster.pending_term == 3U);
    TEST_ASSERT(head->cluster.transition_reason ==
                UCN_CLUSTER_REASON_HIGHER_AUTHORITY);
    return 0;
}

/* CLV2-M03 (03-04): a same-Cluster higher-Term stable Head offer is an RX
 * priority rule, not a collection of role-local comparisons.  Exercise the
 * real decode -> replay admission -> pre-dispatch path for every Current FSM
 * phase that holds active authority/membership.  They have two legitimate
 * destinations (Head identity phases orderly yield; Member/Backup phases
 * begin a join), but every committed transition must have the one explicit
 * HIGHER_AUTHORITY reason.  Foreign and same-Term cases remain 03-03/03-05
 * concerns and are intentionally absent here. */
static int cluster_test_global_higher_authority_pre_dispatch(void)
{
    static const struct {
        ucn_cluster_phase_t phase;
        bool yields_head;
    } cases[] = {
        { UCN_CLUSTER_PHASE_MEMBER_ACTIVE, false },
        { UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE, false },
        { UCN_CLUSTER_PHASE_HEAD_NO_BACKUP, true },
        { UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING, true },
        { UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING, true },
        { UCN_CLUSTER_PHASE_HEAD_STABLE, true },
        { UCN_CLUSTER_PHASE_BACKUP_SYNCING, false },
        { UCN_CLUSTER_PHASE_BACKUP_READY, false },
        { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, false },
        { UCN_CLUSTER_PHASE_STEPPING_DOWN, false },
        { UCN_CLUSTER_PHASE_RECOVERY_HEAD, true },
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        cluster_test_network_t network;
        cluster_test_node_t *node;
        ucn_cluster_message_t message;
        uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
        ucn_node_id_t source;

        TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
        network.now_ms = 100U;
        node = &network.nodes[0];
        source = network.nodes[1].node_id;
        /* Common active Epoch: A/1 is held by the local phase; remote A/2
         * is a protected stable Head proof.  Node 3 is the old primary for
         * Member/Backup staging so the offer really changes authority. */
        node->cluster.role_since_ms = 0U;
        node->cluster.cluster_id = 1U;
        node->cluster.term = 1U;
        node->cluster.head_node_id = network.nodes[2].node_id;
        node->cluster.current_head_score = node->cluster.config.head_score;
        node->cluster.head_grace_deadline_ms = 0U;
        node->cluster.backup_node_id = 0U;
        node->cluster.backup_ready = false;
        node->cluster.backup_syncing = false;
        node->cluster.backup_assign_pending = false;
        node->cluster.backup_takeover_active = false;
        node->cluster.backup_primary_node_id = network.nodes[2].node_id;
        node->cluster.recovery_eligible = false;
        node->cluster.shadow_phase = cases[index].phase;
        node->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
        node->cluster.shadow_transition_count = 0U;

        switch (cases[index].phase) {
            case UCN_CLUSTER_PHASE_MEMBER_ACTIVE:
                node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
                break;
            case UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE:
                node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
                node->cluster.head_grace_deadline_ms = 200U;
                break;
            case UCN_CLUSTER_PHASE_HEAD_NO_BACKUP:
                node->cluster.role = UCN_CLUSTER_ROLE_HEAD;
                node->cluster.head_node_id = node->node_id;
                break;
            case UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING:
                node->cluster.role = UCN_CLUSTER_ROLE_HEAD;
                node->cluster.head_node_id = node->node_id;
                node->cluster.backup_node_id = network.nodes[2].node_id;
                node->cluster.backup_assign_pending = true;
                break;
            case UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING:
                node->cluster.role = UCN_CLUSTER_ROLE_HEAD;
                node->cluster.head_node_id = node->node_id;
                node->cluster.backup_node_id = network.nodes[2].node_id;
                break;
            case UCN_CLUSTER_PHASE_HEAD_STABLE:
                node->cluster.role = UCN_CLUSTER_ROLE_HEAD;
                node->cluster.head_node_id = node->node_id;
                node->cluster.backup_node_id = network.nodes[2].node_id;
                node->cluster.backup_ready = true;
                break;
            case UCN_CLUSTER_PHASE_BACKUP_SYNCING:
                node->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
                node->cluster.backup_syncing = true;
                break;
            case UCN_CLUSTER_PHASE_BACKUP_READY:
                node->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
                node->cluster.backup_ready = true;
                break;
            case UCN_CLUSTER_PHASE_BACKUP_TAKEOVER:
                node->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
                node->cluster.backup_takeover_active = true;
                node->cluster.backup_syncing = true;
                break;
            case UCN_CLUSTER_PHASE_STEPPING_DOWN:
                node->cluster.role = UCN_CLUSTER_ROLE_STEPPING_DOWN;
                node->cluster.pending_head_node_id = network.nodes[2].node_id;
                node->cluster.pending_cluster_id = 1U;
                node->cluster.pending_term = 1U;
                node->cluster.stepdown_deadline_ms = 200U;
                break;
            case UCN_CLUSTER_PHASE_RECOVERY_HEAD:
                node->cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
                node->cluster.head_node_id = node->node_id;
                node->cluster.recovery_eligible = true;
                break;
            default:
                TEST_ASSERT(false);
                return 1;
        }
        TEST_ASSERT(test_derive_phase(&node->cluster, network.now_ms) ==
                    cases[index].phase);

        (void)memset(&message, 0, sizeof(message));
        message.type = UCN_CLUSTER_MSG_ADVERTISE;
        message.role = UCN_CLUSTER_ROLE_HEAD;
        message.cluster_id = 1U;
        message.term = 2U;
        message.head_node_id = source;
        message.head_score = 9000U;
        message.available_capacity = 4U;
        message.lease_ms = 8000U;
        message.nonce = 1U;
        TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        TEST_ASSERT(ucn_cluster_receive(&node->cluster, source, true,
                                        encoded, sizeof(encoded)) == UCN_OK);
        TEST_ASSERT(node->cluster.transition_reason ==
                    UCN_CLUSTER_REASON_HIGHER_AUTHORITY);
        TEST_ASSERT(node->cluster.shadow_transition_count == 1U);
        TEST_ASSERT(node->cluster.pending_head_node_id == source);
        TEST_ASSERT(node->cluster.pending_cluster_id == 1U);
        TEST_ASSERT(node->cluster.pending_term == 2U);
        if (cases[index].yields_head) {
            TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_STEPPING_DOWN);
            TEST_ASSERT(node->cluster.shadow_phase ==
                        UCN_CLUSTER_PHASE_STEPPING_DOWN);
        } else {
            TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
            TEST_ASSERT(node->cluster.shadow_phase ==
                        UCN_CLUSTER_PHASE_JOIN_PENDING);
        }
    }
    return 0;
}

/* CLV2-M03 (03-05): the real RX pre-dispatch must treat two different Head
 * identities for one active (cluster_id, term) as a safety event in every
 * active Current-FSM phase.  Score, Node ID and the old role-local handlers
 * are deliberately absent: every case enters the local-only wait exactly
 * once; Step sends no control traffic; unrelated lifecycle frames are
 * rejected; repeated same-Term Head offers cannot make progress. */
static int cluster_test_global_term_conflict_pre_dispatch(void)
{
    static const ucn_cluster_phase_t cases[] = {
        UCN_CLUSTER_PHASE_ELECTION,
        UCN_CLUSTER_PHASE_JOIN_PENDING,
        UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
        UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE,
        UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
        UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING,
        UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING,
        UCN_CLUSTER_PHASE_HEAD_STABLE,
        UCN_CLUSTER_PHASE_BACKUP_SYNCING,
        UCN_CLUSTER_PHASE_BACKUP_READY,
        UCN_CLUSTER_PHASE_BACKUP_TAKEOVER,
        UCN_CLUSTER_PHASE_RECOVERY_HEAD,
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        cluster_test_network_t network;
        cluster_test_node_t *node;
        ucn_cluster_message_t message;
        uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
        ucn_node_id_t source;

        TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
        network.now_ms = 100U;
        node = &network.nodes[0];
        source = network.nodes[1].node_id;
        node->cluster.role_since_ms = 0U;
        node->cluster.cluster_id = 1U;
        node->cluster.term = 1U;
        node->cluster.head_node_id = network.nodes[2].node_id;
        node->cluster.current_head_score = node->cluster.config.head_score;
        node->cluster.head_grace_deadline_ms = 0U;
        node->cluster.backup_node_id = 0U;
        node->cluster.backup_ready = false;
        node->cluster.backup_syncing = false;
        node->cluster.backup_assign_pending = false;
        node->cluster.backup_takeover_active = false;
        node->cluster.backup_primary_node_id = network.nodes[2].node_id;
        node->cluster.recovery_eligible = false;
        node->cluster.shadow_phase = cases[index];
        node->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
        node->cluster.shadow_transition_count = 0U;

        switch (cases[index]) {
            case UCN_CLUSTER_PHASE_ELECTION:
                node->cluster.role = UCN_CLUSTER_ROLE_CANDIDATE;
                node->cluster.head_node_id = node->node_id;
                node->cluster.election_deadline_ms = 200U;
                break;
            case UCN_CLUSTER_PHASE_JOIN_PENDING:
                node->cluster.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
                break;
            case UCN_CLUSTER_PHASE_MEMBER_ACTIVE:
                node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
                break;
            case UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE:
                node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
                node->cluster.head_grace_deadline_ms = 200U;
                break;
            case UCN_CLUSTER_PHASE_HEAD_NO_BACKUP:
                node->cluster.role = UCN_CLUSTER_ROLE_HEAD;
                node->cluster.head_node_id = node->node_id;
                break;
            case UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING:
                node->cluster.role = UCN_CLUSTER_ROLE_HEAD;
                node->cluster.head_node_id = node->node_id;
                node->cluster.backup_node_id = network.nodes[2].node_id;
                node->cluster.backup_assign_pending = true;
                break;
            case UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING:
                node->cluster.role = UCN_CLUSTER_ROLE_HEAD;
                node->cluster.head_node_id = node->node_id;
                node->cluster.backup_node_id = network.nodes[2].node_id;
                break;
            case UCN_CLUSTER_PHASE_HEAD_STABLE:
                node->cluster.role = UCN_CLUSTER_ROLE_HEAD;
                node->cluster.head_node_id = node->node_id;
                node->cluster.backup_node_id = network.nodes[2].node_id;
                node->cluster.backup_ready = true;
                break;
            case UCN_CLUSTER_PHASE_BACKUP_SYNCING:
                node->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
                node->cluster.backup_syncing = true;
                break;
            case UCN_CLUSTER_PHASE_BACKUP_READY:
                node->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
                node->cluster.backup_ready = true;
                break;
            case UCN_CLUSTER_PHASE_BACKUP_TAKEOVER:
                node->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
                node->cluster.backup_takeover_active = true;
                node->cluster.backup_syncing = true;
                break;
            case UCN_CLUSTER_PHASE_RECOVERY_HEAD:
                node->cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
                node->cluster.head_node_id = node->node_id;
                node->cluster.recovery_eligible = true;
                break;
            default:
                TEST_ASSERT(false);
                return 1;
        }
        TEST_ASSERT(test_derive_phase(&node->cluster, network.now_ms) ==
                    cases[index]);

        (void)memset(&message, 0, sizeof(message));
        message.type = UCN_CLUSTER_MSG_ADVERTISE;
        message.role = UCN_CLUSTER_ROLE_HEAD;
        message.cluster_id = 1U;
        message.term = 1U;
        message.head_node_id = source;
        message.head_score = UCN_CLUSTER_SCORE_MAX;
        message.available_capacity = 4U;
        message.lease_ms = 8000U;
        message.nonce = 1U;
        TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        TEST_ASSERT(ucn_cluster_receive(&node->cluster, source, true,
                                        encoded, sizeof(encoded)) == UCN_OK);
        TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_TERM_CONFLICT);
        TEST_ASSERT(node->cluster.shadow_phase ==
                    UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT);
        TEST_ASSERT(node->cluster.transition_reason ==
                    UCN_CLUSTER_REASON_TERM_CONFLICT);
        TEST_ASSERT(node->cluster.shadow_transition_count == 1U);
        TEST_ASSERT(node->cluster.stats.head_switches == 0U);
        TEST_ASSERT(node->cluster.pending_head_node_id == 0U);
        TEST_ASSERT(node->cluster.pending_cluster_id == 0U);
        TEST_ASSERT(node->cluster.pending_term == 0U);
        TEST_ASSERT(ucn_cluster_step(&node->cluster) == UCN_OK);
        TEST_ASSERT(network.queue_count == 0U);

        /* No voting, takeover or lifecycle frame is accepted while held. */
        message.type = UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT;
        message.nonce = 2U;
        TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        TEST_ASSERT(ucn_cluster_receive(&node->cluster, source, true,
                                        encoded, sizeof(encoded)) ==
                    UCN_ERR_STATE);
        TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_TERM_CONFLICT);
        TEST_ASSERT(node->cluster.shadow_transition_count == 1U);

        /* Same-Term replay/new score is inert, rather than score arbitration. */
        message.type = UCN_CLUSTER_MSG_ADVERTISE;
        message.head_score = 1U;
        message.nonce = 3U;
        TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        TEST_ASSERT(ucn_cluster_receive(&node->cluster, source, true,
                                        encoded, sizeof(encoded)) == UCN_OK);
        TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_TERM_CONFLICT);
        TEST_ASSERT(node->cluster.shadow_transition_count == 1U);
    }
    return 0;
}

/* M03 audit minor closure: pin the Candidate sequence explicitly instead of
 * relying on the broad multi-phase matrix.  A lower remote Epoch must not
 * perturb the local election; a later same-cluster higher Epoch must use the
 * one global higher-authority path and become JOIN_PENDING. */
static int cluster_test_candidate_lower_authority_sequence(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_node_id_t source;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[0];
    source = network.nodes[1].node_id;
    node->cluster.role = UCN_CLUSTER_ROLE_CANDIDATE;
    node->cluster.cluster_id = 1U;
    node->cluster.term = 5U;
    node->cluster.head_node_id = node->node_id;
    node->cluster.election_deadline_ms = 200U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_ELECTION;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    node->cluster.shadow_transition_count = 0U;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 4U;
    message.head_node_id = source;
    message.head_score = UCN_CLUSTER_SCORE_MAX;
    message.available_capacity = 4U;
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, source, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_CANDIDATE);
    TEST_ASSERT(node->cluster.shadow_phase == UCN_CLUSTER_PHASE_ELECTION);
    TEST_ASSERT(node->cluster.term == 5U);
    TEST_ASSERT(node->cluster.shadow_transition_count == 0U);
    TEST_ASSERT(network.queue_count == 0U);

    message.term = 6U;
    message.nonce = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, source, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(node->cluster.shadow_phase == UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(node->cluster.transition_reason ==
                UCN_CLUSTER_REASON_HIGHER_AUTHORITY);
    TEST_ASSERT(node->cluster.pending_head_node_id == source);
    TEST_ASSERT(node->cluster.pending_cluster_id == 1U);
    TEST_ASSERT(node->cluster.pending_term == 6U);
    TEST_ASSERT(node->cluster.shadow_transition_count == 1U);
    return 0;
}

/* M03 audit minor closure: the legality-table edge must also be proven by
 * real RX.  A stepping-down Head that receives a competing same-Term Head
 * cannot finish its ordered transition or resume control activity. */
static int cluster_test_stepdown_term_conflict_rx(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_node_id_t source;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[0];
    source = network.nodes[1].node_id;
    node->cluster.role = UCN_CLUSTER_ROLE_STEPPING_DOWN;
    node->cluster.cluster_id = 1U;
    node->cluster.term = 5U;
    node->cluster.head_node_id = node->node_id;
    node->cluster.pending_cluster_id = 1U;
    node->cluster.pending_term = 5U;
    node->cluster.pending_head_node_id = network.nodes[2].node_id;
    node->cluster.stepdown_deadline_ms = 200U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_STEPPING_DOWN;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_STEPDOWN_ORDERED;
    node->cluster.shadow_transition_count = 0U;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 5U;
    message.head_node_id = source;
    message.head_score = UCN_CLUSTER_SCORE_MAX;
    message.available_capacity = 4U;
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, source, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_TERM_CONFLICT);
    TEST_ASSERT(node->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT);
    TEST_ASSERT(node->cluster.transition_reason ==
                UCN_CLUSTER_REASON_TERM_CONFLICT);
    TEST_ASSERT(node->cluster.stepdown_deadline_ms == 0U);
    TEST_ASSERT(node->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(ucn_cluster_step(&node->cluster) == UCN_OK);
    TEST_ASSERT(network.queue_count == 0U);
    return 0;
}

/* CLV2-M03 (03-07): no Cluster safety serial may wrap inside one identity.
 * Until M13 Rekey exists, Term exhaustion is an explicit public error and
 * leaves the pre-existing role/epoch/transition untouched.  Backup
 * generation exhaustion likewise leaves the Head without a new Backup,
 * rather than reusing generation 1. */
static int cluster_test_serial_exhaustion_fails_closed(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_t *cluster;
    uint32_t transition_count;

    /* A fresh Cluster identity must also never reuse its allocation round.
     * This fails before the DETACHED node can transition or advertise. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[0];
    cluster = &node->cluster;
    cluster->role = UCN_CLUSTER_ROLE_DETACHED;
    cluster->observation_deadline_ms = 1U;
    cluster->cluster_id_round = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    cluster->shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE;
    cluster->transition_reason = UCN_CLUSTER_REASON_INIT;
    transition_count = cluster->shadow_transition_count;
    TEST_ASSERT(ucn_cluster_step(cluster) == UCN_ERR_EXHAUSTED);
    TEST_ASSERT(cluster->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(cluster->cluster_id == 0U && cluster->term == 0U);
    TEST_ASSERT(cluster->shadow_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(cluster->shadow_transition_count == transition_count);
    TEST_ASSERT(network.queue_count == 0U);

    /* A score challenge cannot turn a maximum safe Term back into 1. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[1];
    cluster = &node->cluster;
    cluster->role = UCN_CLUSTER_ROLE_BACKUP;
    cluster->cluster_id = 1U;
    cluster->term = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    cluster->head_node_id = network.nodes[0].node_id;
    cluster->backup_primary_node_id = network.nodes[0].node_id;
    cluster->backup_ready = true;
    cluster->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;
    cluster->transition_reason = UCN_CLUSTER_REASON_INIT;
    transition_count = cluster->shadow_transition_count;
    TEST_ASSERT(ucn_cluster_test_backup_challenge(cluster, network.now_ms) ==
                UCN_ERR_UNSUPPORTED);
    TEST_ASSERT(cluster->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(cluster->term == UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD);
    TEST_ASSERT(cluster->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_READY);
    TEST_ASSERT(cluster->shadow_transition_count == transition_count);

    /* Majority ACK cannot promote a Backup by wrapping Term either. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[1];
    cluster = &node->cluster;
    cluster->role = UCN_CLUSTER_ROLE_BACKUP;
    cluster->cluster_id = 1U;
    cluster->term = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    cluster->head_node_id = network.nodes[0].node_id;
    cluster->backup_takeover_active = true;
    cluster->backup_syncing = true;
    cluster->shadow_phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
    cluster->transition_reason = UCN_CLUSTER_REASON_INIT;
    transition_count = cluster->shadow_transition_count;
    TEST_ASSERT(ucn_cluster_test_complete_takeover(cluster, network.now_ms) ==
                UCN_ERR_EXHAUSTED);
    TEST_ASSERT(cluster->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(cluster->term == UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD);
    TEST_ASSERT(cluster->backup_takeover_active == true);
    TEST_ASSERT(cluster->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    TEST_ASSERT(cluster->shadow_transition_count == transition_count);

    /* Backup generation uses the same no-wrap gate.  The assignment stays
     * uncommitted, so no peer can interpret a fresh Backup as generation 1. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[0];
    cluster = &node->cluster;
    cluster->role = UCN_CLUSTER_ROLE_HEAD;
    cluster->cluster_id = node->node_id;
    cluster->term = 1U;
    cluster->head_node_id = node->node_id;
    cluster->backup_generation = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    cluster->shadow_phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    cluster->transition_reason = UCN_CLUSTER_REASON_INIT;
    cluster->primary_members.slots[0].occupied = true;
    cluster->primary_members.slots[0].node_id = network.nodes[1].node_id;
    cluster->candidates[0].occupied = true;
    cluster->candidates[0].head_node_id = network.nodes[1].node_id;
    cluster->candidates[0].head_score = 7000U;
    transition_count = cluster->shadow_transition_count;
    ucn_cluster_test_assign_backup(cluster, network.now_ms);
    TEST_ASSERT(cluster->backup_node_id == 0U);
    TEST_ASSERT(cluster->backup_generation ==
                UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD);
    TEST_ASSERT(cluster->shadow_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    TEST_ASSERT(cluster->shadow_transition_count == transition_count);

    /* The running Backup mirror cannot emit a wrapped membership sequence.
     * The owner Step reports the same explicit exhaustion to its caller. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[0];
    cluster = &node->cluster;
    cluster->role = UCN_CLUSTER_ROLE_HEAD;
    cluster->cluster_id = node->node_id;
    cluster->term = 1U;
    cluster->head_node_id = node->node_id;
    cluster->backup_node_id = network.nodes[1].node_id;
    cluster->backup_generation = 1U;
    cluster->backup_ready = true;
    cluster->membership_sequence = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    cluster->next_backup_assign_ms = 1000U;
    cluster->next_backup_delta_ms = 1U;
    cluster->shadow_phase = UCN_CLUSTER_PHASE_HEAD_STABLE;
    cluster->transition_reason = UCN_CLUSTER_REASON_INIT;
    cluster->primary_members.slots[0].occupied = true;
    cluster->primary_members.slots[0].node_id = network.nodes[1].node_id;
    cluster->primary_members.slots[0].lease_expires_at_ms = 1000U;
    TEST_ASSERT(ucn_cluster_step(cluster) == UCN_ERR_EXHAUSTED);
    TEST_ASSERT(cluster->membership_sequence ==
                UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD);
    TEST_ASSERT(cluster->role == UCN_CLUSTER_ROLE_HEAD);
    TEST_ASSERT(cluster->shadow_phase == UCN_CLUSTER_PHASE_HEAD_STABLE);
    return 0;
}

/* CLV2-M03 (03-08): Cluster IDs are allocated through a small optional
 * Provider contract.  The provider receives boot incarnation plus a local
 * monotonic round, while the core rejects zero/broadcast/parent reuse before
 * committing an Election or Recovery transition. */
static int cluster_test_cluster_id_provider(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_t *cluster;
    ucn_cluster_config_t config;
    cluster_test_id_provider_t provider;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[0];
    config = node->cluster.config;
    (void)memset(&provider, 0, sizeof(provider));
    provider.ids[0] = UINT32_C(0x13572468);
    provider.ids[1] = UINT32_C(0x24681357);
    provider.supplied = 2U;
    provider.result = UCN_OK;
    config.make_cluster_id = cluster_test_make_id;
    config.cluster_id_context = &provider;
    config.cluster_id_incarnation = UINT32_C(7);
    TEST_ASSERT(ucn_cluster_init(&node->cluster, &config) == UCN_OK);
    cluster = &node->cluster;
    cluster->observation_deadline_ms = 1U;
    TEST_ASSERT(ucn_cluster_step(cluster) == UCN_OK);
    TEST_ASSERT(cluster->role == UCN_CLUSTER_ROLE_CANDIDATE);
    TEST_ASSERT(cluster->cluster_id == provider.ids[0]);
    TEST_ASSERT(cluster->term == 1U);
    TEST_ASSERT(cluster->cluster_id_round == 1U);
    TEST_ASSERT(provider.count == 1U);
    TEST_ASSERT(provider.requests[0].purpose ==
                UCN_CLUSTER_ID_PURPOSE_ELECTION);
    TEST_ASSERT(provider.requests[0].local_node_id == node->node_id);
    TEST_ASSERT(provider.requests[0].parent_cluster_id == 0U);
    TEST_ASSERT(provider.requests[0].parent_term == 0U);
    TEST_ASSERT(provider.requests[0].incarnation == UINT32_C(7));
    TEST_ASSERT(provider.requests[0].round == 1U);

    /* Simulate a completed old Cluster being detached.  The next ordinary
     * Cluster carries its prior identity as parent and gets a fresh ID/Term. */
    cluster->role = UCN_CLUSTER_ROLE_DETACHED;
    cluster->cluster_id = 0U;
    cluster->term = 0U;
    cluster->head_node_id = 0U;
    cluster->last_cluster_id = provider.ids[0];
    cluster->max_seen_term = 1U;
    cluster->last_stable_head = node->node_id;
    cluster->observation_deadline_ms = 1U;
    cluster->shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE;
    cluster->transition_reason = UCN_CLUSTER_REASON_INIT;
    cluster->shadow_transition_count = 0U;
    TEST_ASSERT(ucn_cluster_step(cluster) == UCN_OK);
    TEST_ASSERT(cluster->cluster_id == provider.ids[1]);
    TEST_ASSERT(cluster->cluster_id != provider.ids[0]);
    TEST_ASSERT(cluster->term == 1U);
    TEST_ASSERT(cluster->cluster_id_round == 2U);
    TEST_ASSERT(provider.count == 2U);
    TEST_ASSERT(provider.requests[1].parent_cluster_id == provider.ids[0]);
    TEST_ASSERT(provider.requests[1].parent_term == 1U);
    TEST_ASSERT(provider.requests[1].round == 2U);

    /* Invalid Provider output does not consume the local round or mutate
     * the detached state. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[0];
    config = node->cluster.config;
    (void)memset(&provider, 0, sizeof(provider));
    provider.ids[0] = 0U;
    provider.supplied = 1U;
    provider.result = UCN_OK;
    config.make_cluster_id = cluster_test_make_id;
    config.cluster_id_context = &provider;
    TEST_ASSERT(ucn_cluster_init(&node->cluster, &config) == UCN_OK);
    cluster = &node->cluster;
    cluster->observation_deadline_ms = 1U;
    TEST_ASSERT(ucn_cluster_step(cluster) == UCN_ERR_CONFIG);
    TEST_ASSERT(cluster->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(cluster->cluster_id == 0U && cluster->term == 0U);
    TEST_ASSERT(cluster->cluster_id_round == 0U);

    /* The core validates every reserved/unsafe callback value itself: the
     * callback cannot publish broadcast or resurrect the parent Cluster. */
    provider.ids[0] = UCN_NODE_BROADCAST;
    provider.count = 0U;
    TEST_ASSERT(ucn_cluster_init(&node->cluster, &config) == UCN_OK);
    cluster = &node->cluster;
    cluster->observation_deadline_ms = 1U;
    TEST_ASSERT(ucn_cluster_step(cluster) == UCN_ERR_CONFIG);
    TEST_ASSERT(cluster->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(cluster->cluster_id_round == 0U);

    provider.ids[0] = UINT32_C(9);
    provider.count = 0U;
    TEST_ASSERT(ucn_cluster_init(&node->cluster, &config) == UCN_OK);
    cluster = &node->cluster;
    cluster->last_cluster_id = provider.ids[0];
    cluster->max_seen_term = 1U;
    cluster->last_stable_head = network.nodes[1].node_id;
    cluster->observation_deadline_ms = 1U;
    TEST_ASSERT(ucn_cluster_step(cluster) == UCN_ERR_CONFIG);
    TEST_ASSERT(cluster->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(cluster->cluster_id_round == 0U);

    /* The deterministic default retains the first legacy election ID, then
     * allocates a new identity for a detached successor of that Cluster. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[0];
    cluster = &node->cluster;
    cluster->observation_deadline_ms = 1U;
    TEST_ASSERT(ucn_cluster_step(cluster) == UCN_OK);
    TEST_ASSERT(cluster->cluster_id == node->node_id);
    cluster->role = UCN_CLUSTER_ROLE_DETACHED;
    cluster->last_cluster_id = cluster->cluster_id;
    cluster->max_seen_term = cluster->term;
    cluster->last_stable_head = node->node_id;
    cluster->cluster_id = 0U;
    cluster->term = 0U;
    cluster->head_node_id = 0U;
    cluster->observation_deadline_ms = 1U;
    cluster->shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE;
    cluster->transition_reason = UCN_CLUSTER_REASON_INIT;
    TEST_ASSERT(ucn_cluster_step(cluster) == UCN_OK);
    TEST_ASSERT(cluster->cluster_id != 0U);
    TEST_ASSERT(cluster->cluster_id != node->node_id);
    TEST_ASSERT(cluster->cluster_id != cluster->last_cluster_id);

    /* Even a failed candidate that has not yet become a stable Cluster has
     * no history parent.  Its next local allocation round must still not
     * reuse the legacy first-round Node ID. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[0];
    cluster = &node->cluster;
    cluster->observation_deadline_ms = 1U;
    TEST_ASSERT(ucn_cluster_step(cluster) == UCN_OK);
    TEST_ASSERT(cluster->cluster_id == node->node_id);
    cluster->role = UCN_CLUSTER_ROLE_DETACHED;
    cluster->cluster_id = 0U;
    cluster->term = 0U;
    cluster->head_node_id = 0U;
    cluster->observation_deadline_ms = 1U;
    cluster->shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE;
    cluster->transition_reason = UCN_CLUSTER_REASON_INIT;
    TEST_ASSERT(ucn_cluster_step(cluster) == UCN_OK);
    TEST_ASSERT(cluster->cluster_id_round == 2U);
    TEST_ASSERT(cluster->cluster_id != 0U);
    TEST_ASSERT(cluster->cluster_id != node->node_id);
    return 0;
}

/* CLV2-M03 (03-06): Detach must clear only active/pending state.  The last
 * stable Cluster identity survives in RAM so delayed old-Term advertisements
 * cannot make a detached node rejoin; an exact remembered Head/Term remains
 * joinable.  A received higher Term is remembered before its local join
 * procedure completes, so a later detach cannot regress to the old one. */
static int cluster_test_detach_history_rejects_old_term(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_node_id_t old_head;
    ucn_node_id_t other_head;
    ucn_cluster_message_t message;
    ucn_cluster_view_t view;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[0];
    old_head = network.nodes[1].node_id;
    other_head = network.nodes[2].node_id;
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    node->cluster.cluster_id = 9U;
    node->cluster.term = 7U;
    node->cluster.head_node_id = old_head;
    node->cluster.head_lease_expires_at_ms = 1000U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    node->cluster.shadow_transition_count = 0U;

    /* A real current-epoch stepdown detaches the Member and records A/7. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_STEPDOWN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 9U;
    message.term = 7U;
    message.head_node_id = old_head;
    message.head_score = 4000U;
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, old_head, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(node->cluster.cluster_id == 0U);
    TEST_ASSERT(node->cluster.term == 0U);
    TEST_ASSERT(node->cluster.head_node_id == 0U);
    TEST_ASSERT(node->cluster.last_cluster_id == 9U);
    TEST_ASSERT(node->cluster.max_seen_term == 7U);
    TEST_ASSERT(node->cluster.last_stable_head == old_head);
    TEST_ASSERT(ucn_cluster_get_view(&node->cluster, &view) == UCN_OK);
    TEST_ASSERT(view.cluster_id == 0U && view.term == 0U);
    TEST_ASSERT(view.last_cluster_id == 9U);
    TEST_ASSERT(view.max_seen_term == 7U);
    TEST_ASSERT(view.last_stable_head == old_head);

    /* A delayed A/6 cannot create a new join transaction. */
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.cluster_id = 9U;
    message.term = 6U;
    message.head_node_id = other_head;
    message.head_score = 9000U;
    message.available_capacity = 4U;
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, other_head, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(node->cluster.pending_head_node_id == 0U);

    /* A competing A/7 Head is also not a safe detached rejoin target. */
    message.term = 7U;
    message.nonce = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, other_head, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(node->cluster.pending_head_node_id == 0U);

    /* Exact A/7/old_head is not stale and may start a normal rejoin. */
    message.head_node_id = old_head;
    message.nonce = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, old_head, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(node->cluster.pending_cluster_id == 9U);
    TEST_ASSERT(node->cluster.pending_term == 7U);
    TEST_ASSERT(node->cluster.pending_head_node_id == old_head);

    /* A higher proof is remembered before JOIN_ACCEPT.  When the old Head
     * then steps down the in-flight join, Detach keeps A/8 rather than
     * overwriting it with the old active A/7 identity. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[0];
    old_head = network.nodes[1].node_id;
    other_head = network.nodes[2].node_id;
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    node->cluster.cluster_id = 9U;
    node->cluster.term = 7U;
    node->cluster.head_node_id = old_head;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    node->cluster.shadow_transition_count = 0U;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 9U;
    message.term = 8U;
    message.head_node_id = other_head;
    message.head_score = 9000U;
    message.available_capacity = 4U;
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, other_head, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(node->cluster.last_cluster_id == 9U);
    TEST_ASSERT(node->cluster.max_seen_term == 8U);
    TEST_ASSERT(node->cluster.last_stable_head == other_head);

    message.type = UCN_CLUSTER_MSG_HEAD_STEPDOWN;
    message.term = 7U;
    message.head_node_id = old_head;
    message.head_score = 4000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, old_head, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(node->cluster.max_seen_term == 8U);
    TEST_ASSERT(node->cluster.last_stable_head == other_head);

    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.nonce = 3U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, old_head, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    return 0;
}

/* CLV2-M12 (12-01): recovery lineage capture.  The Member grace-timeout
 * and Backup missed-heartbeat fence exits capture parent_cluster_id /
 * parent_term / parent_config_id BEFORE the detach clears the identity;
 * the lineage survives detach; same-parent re-entry preserves the round
 * and upgrades the term; a new parent replaces the lineage and resets the
 * round; a recovery domain itself is never a parent; the view exposes the
 * snapshot. */
static int cluster_test_recovery_lineage_capture(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_view_t view;
    ucn_node_id_t old_head;

    /* ---- (a) Member grace-timeout fence exit captures the live identity. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[0];
    old_head = network.nodes[1].node_id;
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    node->cluster.cluster_id = 9U;
    node->cluster.term = 7U;
    node->cluster.head_node_id = old_head;
    /* The grace-timeout site only fires once BOTH the member lease and the
     * grace deadline are expired (the lease lapse arms grace first). */
    node->cluster.head_lease_expires_at_ms = 50U;
    node->cluster.head_grace_deadline_ms = 50U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    node->cluster.shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(&node->cluster, 100U) ==
                UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE);
    TEST_ASSERT(ucn_cluster_step(&node->cluster) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(node->cluster.cluster_id == 0U);
    TEST_ASSERT(node->cluster.term == 0U);
    TEST_ASSERT(node->cluster.recovery_eligible == true);
    /* Lineage captured BEFORE the detach: A/9 term 7, no Config owner. */
    TEST_ASSERT(node->cluster.parent_cluster_id == 9U);
    TEST_ASSERT(node->cluster.parent_term == 7U);
    TEST_ASSERT(node->cluster.parent_config_id == 0U);
    TEST_ASSERT(node->cluster.recovery_round == 0U);
    TEST_ASSERT(ucn_cluster_get_view(&node->cluster, &view) == UCN_OK);
    TEST_ASSERT(view.parent_cluster_id == 9U);
    TEST_ASSERT(view.parent_term == 7U);
    TEST_ASSERT(view.parent_config_id == 0U);
    TEST_ASSERT(view.recovery_round == 0U);

    /* ---- (b) Backup missed-heartbeat fence exit captures the identity. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[1];
    old_head = network.nodes[0].node_id;
    node->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
    node->cluster.cluster_id = 9U;
    node->cluster.term = 7U;
    node->cluster.head_node_id = old_head;
    node->cluster.backup_primary_node_id = old_head;
    node->cluster.backup_primary_deadline_ms = 50U;
    node->cluster.backup_missed_heartbeats = UCN_CLUSTER_BACKUP_MISS_LIMIT - 1U;
    node->cluster.backup_ready = false;
    node->cluster.backup_syncing = true;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_SYNCING;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    node->cluster.shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(&node->cluster, 100U) ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(ucn_cluster_step(&node->cluster) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(node->cluster.recovery_eligible == true);
    TEST_ASSERT(node->cluster.parent_cluster_id == 9U);
    TEST_ASSERT(node->cluster.parent_term == 7U);
    TEST_ASSERT(node->cluster.recovery_round == 0U);

    /* ---- (c1) Same-parent re-entry preserves the round, upgrades term. */
    node->cluster.recovery_round = 3U; /* failed attempts so far (12-03) */
    node->cluster.cluster_id = 0U;     /* detach-cycle identity */
    node->cluster.term = 0U;
    node->cluster.max_seen_term = 8U;  /* a higher proof arrived meanwhile */
    ucn_cluster_test_lineage_capture(&node->cluster);
    TEST_ASSERT(node->cluster.parent_cluster_id == 9U);
    TEST_ASSERT(node->cluster.parent_term == 8U);
    TEST_ASSERT(node->cluster.recovery_round == 3U);

    /* ---- (c2) A different parent replaces the lineage, resets the round
     * and consumes the bound config_id. */
    ucn_cluster_lineage_bind_config(&node->cluster, 12U);
    node->cluster.last_cluster_id = 4U;
    node->cluster.max_seen_term = 2U;
    node->cluster.last_stable_head = network.nodes[2].node_id;
    ucn_cluster_test_lineage_capture(&node->cluster);
    TEST_ASSERT(node->cluster.parent_cluster_id == 4U);
    TEST_ASSERT(node->cluster.parent_term == 2U);
    TEST_ASSERT(node->cluster.parent_config_id == 12U);
    TEST_ASSERT(node->cluster.recovery_round == 0U);

    /* ---- (c4) CLV2-M12.1 (MAJOR-3): same-parent config lineage is
     * forward-only.  A freshly bound higher config_id upgrades the
     * capture (round preserved); a stale binding never regresses it. */
    ucn_cluster_lineage_bind_config(&node->cluster, 13U);
    node->cluster.last_cluster_id = 4U; /* same parent as the capture */
    node->cluster.max_seen_term = 2U;
    node->cluster.cluster_id = 4U; /* live same-parent identity */
    node->cluster.term = 2U;
    node->cluster.recovery_cluster_id = 0U;
    node->cluster.recovery_round = 3U;
    ucn_cluster_test_lineage_capture(&node->cluster);
    TEST_ASSERT(node->cluster.parent_cluster_id == 4U);
    TEST_ASSERT(node->cluster.parent_config_id == 13U); /* upgraded 12->13 */
    TEST_ASSERT(node->cluster.recovery_round == 3U); /* round preserved */
    ucn_cluster_lineage_bind_config(&node->cluster, 9U); /* stale binding */
    ucn_cluster_test_lineage_capture(&node->cluster);
    TEST_ASSERT(node->cluster.parent_config_id == 13U); /* never regressed */
    TEST_ASSERT(node->cluster.recovery_round == 3U);

    /* ---- (c3) A recovery domain is never captured as a parent. */
    node->cluster.cluster_id = 55U;
    node->cluster.term = 1U;
    node->cluster.head_node_id = node->node_id;
    node->cluster.recovery_cluster_id = 55U;
    node->cluster.last_cluster_id = 9U;
    node->cluster.max_seen_term = 7U;
    ucn_cluster_test_lineage_capture(&node->cluster);
    TEST_ASSERT(node->cluster.parent_cluster_id != 55U);
    TEST_ASSERT(node->cluster.parent_term != 1U);
    /* The history's stable domain (A/9 term 7) is the parent instead. */
    TEST_ASSERT(node->cluster.parent_cluster_id == 9U);
    TEST_ASSERT(node->cluster.parent_term == 7U);
    TEST_ASSERT(node->cluster.recovery_round == 0U);
    return 0;
}

/* CLV2-M12 (12-02): Recovery ID uniqueness.  The recovery allocation
 * entry carries the full lineage (parent cluster/term/config + recovery
 * round) plus node/incarnation/object round into the Provider request;
 * the default generator derives a distinct ID per round and boot; and an
 * invalid Provider answer (0 / broadcast / parent reuse) is rejected with
 * UCN_ERR_CONFIG without consuming the object round. */
typedef struct recovery_id_probe {
    ucn_cluster_id_request_t request;
    uint32_t answer;
    ucn_result_t result;
    unsigned calls;
} recovery_id_probe_t;

static ucn_result_t recovery_id_probe_fn(void *context,
                                         const ucn_cluster_id_request_t *request,
                                         uint32_t *cluster_id)
{
    recovery_id_probe_t *probe = (recovery_id_probe_t *)context;

    probe->request = *request;
    probe->calls++;
    if (probe->result != UCN_OK) {
        return probe->result;
    }
    *cluster_id = probe->answer;
    return UCN_OK;
}

static int cluster_test_recovery_id_uniqueness(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    recovery_id_probe_t probe;
    uint32_t id1;
    uint32_t id2;
    uint32_t id3;
    uint32_t id4;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    node = &network.nodes[0];

    /* ---- (a) the request carries the full lineage and node identity. */
    (void)memset(&probe, 0, sizeof(probe));
    probe.answer = UINT32_C(0x12345678);
    probe.result = UCN_OK;
    node->cluster.config.make_cluster_id = recovery_id_probe_fn;
    node->cluster.config.cluster_id_context = &probe;
    node->cluster.config.cluster_id_incarnation = 5U;
    TEST_ASSERT(ucn_cluster_test_make_next_recovery_id(
                    &node->cluster, 9U, 7U, 12U, 3U, &id1) == UCN_OK);
    TEST_ASSERT(id1 == UINT32_C(0x12345678));
    TEST_ASSERT(probe.calls == 1U);
    TEST_ASSERT(probe.request.purpose == UCN_CLUSTER_ID_PURPOSE_RECOVERY);
    TEST_ASSERT(probe.request.local_node_id == node->node_id);
    TEST_ASSERT(probe.request.parent_cluster_id == 9U);
    TEST_ASSERT(probe.request.parent_term == 7U);
    TEST_ASSERT(probe.request.parent_config_id == 12U);
    TEST_ASSERT(probe.request.recovery_round == 3U);
    TEST_ASSERT(probe.request.incarnation == 5U);
    TEST_ASSERT(probe.request.round == 1U); /* first allocation */
    TEST_ASSERT(node->cluster.cluster_id_round == 1U);

    /* ---- (b) parent reuse is rejected without consuming the round. */
    probe.answer = 9U; /* == parent_cluster_id */
    TEST_ASSERT(ucn_cluster_test_make_next_recovery_id(
                    &node->cluster, 9U, 7U, 12U, 3U, &id2) == UCN_ERR_CONFIG);
    TEST_ASSERT(node->cluster.cluster_id_round == 1U); /* unconsumed */

    /* ---- (c) zero / broadcast answers are rejected, round unconsumed. */
    probe.answer = 0U;
    TEST_ASSERT(ucn_cluster_test_make_next_recovery_id(
                    &node->cluster, 9U, 7U, 12U, 3U, &id2) == UCN_ERR_CONFIG);
    probe.answer = UCN_NODE_BROADCAST;
    TEST_ASSERT(ucn_cluster_test_make_next_recovery_id(
                    &node->cluster, 9U, 7U, 12U, 3U, &id2) == UCN_ERR_CONFIG);
    TEST_ASSERT(node->cluster.cluster_id_round == 1U);

    /* ---- (d) the default generator derives a distinct ID per recovery
     * round and per boot incarnation, never parent/0/broadcast. */
    node->cluster.config.make_cluster_id = NULL;
    node->cluster.config.cluster_id_context = NULL;
    node->cluster.config.cluster_id_incarnation = 0U;
    TEST_ASSERT(ucn_cluster_test_make_next_recovery_id(
                    &node->cluster, 9U, 7U, 12U, 0U, &id1) == UCN_OK);
    TEST_ASSERT(id1 != 9U && id1 != 0U && id1 != UCN_NODE_BROADCAST);
    TEST_ASSERT(ucn_cluster_test_make_next_recovery_id(
                    &node->cluster, 9U, 7U, 12U, 1U, &id2) == UCN_OK);
    TEST_ASSERT(id2 != id1);
    TEST_ASSERT(id2 != 9U && id2 != 0U && id2 != UCN_NODE_BROADCAST);
    /* Same lineage but a different boot incarnation derives a new ID. */
    node->cluster.config.cluster_id_incarnation = 1U;
    TEST_ASSERT(ucn_cluster_test_make_next_recovery_id(
                    &node->cluster, 9U, 7U, 12U, 0U, &id3) == UCN_OK);
    TEST_ASSERT(id3 != id1 && id3 != id2);
    TEST_ASSERT(id3 != 9U && id3 != 0U && id3 != UCN_NODE_BROADCAST);
    /* A further call (object round advanced) also never repeats. */
    TEST_ASSERT(ucn_cluster_test_make_next_recovery_id(
                    &node->cluster, 9U, 7U, 12U, 0U, &id4) == UCN_OK);
    TEST_ASSERT(id4 != id1 && id4 != id2 && id4 != id3);
    TEST_ASSERT(id4 != 9U && id4 != 0U && id4 != UCN_NODE_BROADCAST);

    /* ---- (e) consecutive recovery rounds with consecutive object rounds
     * (the TTL-failure pattern: rround 0->1->2... while the object round
     * also advances) must never derive a repeating ID.  This pins the
     * chained-mix fix: XOR cancellation is not allowed to collide. */
    {
        uint32_t seq[8];
        size_t si;
        size_t sj;

        node->cluster.config.cluster_id_incarnation = 0U;
        for (si = 0U; si < 8U; ++si) {
            TEST_ASSERT(ucn_cluster_test_make_next_recovery_id(
                            &node->cluster, 9U, 7U, 12U, (uint32_t)si,
                            &seq[si]) == UCN_OK);
            TEST_ASSERT(seq[si] != 9U && seq[si] != 0U &&
                        seq[si] != UCN_NODE_BROADCAST);
        }
        for (si = 0U; si < 8U; ++si) {
            for (sj = si + 1U; sj < 8U; ++sj) {
                TEST_ASSERT(seq[si] != seq[sj]);
            }
        }
    }
    return 0;
}

/* CLV2-M12 (12-03): bounded exponential backoff with deterministic
 * jitter, round escalation on TTL expiry, and the sustained-stable-join
 * lineage reset. */
static int cluster_test_recovery_backoff_and_reset(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_message_t message;
    ucn_cluster_view_t view;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    uint32_t backoff[5];
    uint32_t now_ms = 100U;
    size_t i;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    node = &network.nodes[0];

    /* ---- (a) exponential escalation: non-decreasing across rounds and
     * strictly increasing before the cap; always non-zero.  The fixture's
     * tiny backoff_max would saturate immediately, so stage a realistic
     * base/max pair first. */
    node->cluster.config.recovery_backoff_base_ms = 200U;
    node->cluster.config.recovery_backoff_max_ms = 2000U;
    node->cluster.parent_cluster_id = 9U;
    for (i = 0U; i < 5U; ++i) {
        node->cluster.recovery_round = (uint32_t)i;
        backoff[i] = compute_recovery_backoff(&node->cluster);
        TEST_ASSERT(backoff[i] != 0U);
        TEST_ASSERT(backoff[i] <= node->cluster.config.recovery_backoff_max_ms);
        if (i != 0U) {
            TEST_ASSERT(backoff[i] >= backoff[i - 1U]);
        }
    }
    TEST_ASSERT(backoff[1] > backoff[0]);
    /* Deep rounds stay capped at max. */
    node->cluster.recovery_round = 100U;
    TEST_ASSERT(compute_recovery_backoff(&node->cluster) ==
                node->cluster.config.recovery_backoff_max_ms);
    /* ---- (b) deterministic: identical state computes identical values. */
    node->cluster.recovery_round = 2U;
    TEST_ASSERT(compute_recovery_backoff(&node->cluster) ==
                compute_recovery_backoff(&node->cluster));

    /* ---- (c) TTL expiry escalates the round via the real Step path. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = now_ms;
    node = &network.nodes[0];
    node->cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    node->cluster.cluster_id = 55U;
    node->cluster.recovery_cluster_id = 55U;
    node->cluster.term = 1U;
    node->cluster.head_node_id = node->node_id;
    node->cluster.recovery_deadline_ms = 50U; /* expired at 100 */
    node->cluster.recovery_nonce = 7U;
    node->cluster.recovery_round = 3U;
    node->cluster.parent_cluster_id = 9U;
    node->cluster.parent_term = 7U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    node->cluster.shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(&node->cluster, now_ms) ==
                UCN_CLUSTER_PHASE_RECOVERY_HEAD);
    TEST_ASSERT(ucn_cluster_step(&node->cluster) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(node->cluster.recovery_round == 4U); /* escalated */
    TEST_ASSERT(node->cluster.parent_cluster_id == 9U); /* lineage kept */

    /* ---- (d) a stable JOIN_ACCEPT arms the reset; expiry clears the
     * lineage and round. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = now_ms;
    node = &network.nodes[2];
    node->cluster.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    node->cluster.pending_head_node_id = network.nodes[0].node_id;
    node->cluster.pending_cluster_id = 9U;
    node->cluster.pending_term = 7U;
    node->cluster.pending_join_nonce = 1U;
    node->cluster.parent_cluster_id = 4U;
    node->cluster.parent_term = 2U;
    node->cluster.recovery_round = 6U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_JOIN_PENDING;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_JOIN_INITIATED;
    node->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_JOIN_ACCEPT;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 9U;
    message.term = 7U;
    message.head_node_id = network.nodes[0].node_id;
    message.head_score = 9000U;
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster,
                                    network.nodes[0].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(node->cluster.lineage_reset_deadline_ms != 0U);
    /* Not yet expired: the lineage still survives. */
    TEST_ASSERT(node->cluster.parent_cluster_id == 4U);
    /* Step past the reset period: lineage and round are cleared. */
    network.now_ms = now_ms + node->cluster.config.recovery_lineage_reset_ms +
                     1U;
    TEST_ASSERT(ucn_cluster_step(&node->cluster) == UCN_OK);
    TEST_ASSERT(node->cluster.parent_cluster_id == 0U);
    TEST_ASSERT(node->cluster.parent_term == 0U);
    TEST_ASSERT(node->cluster.parent_config_id == 0U);
    TEST_ASSERT(node->cluster.recovery_round == 0U);
    TEST_ASSERT(node->cluster.lineage_reset_deadline_ms == 0U);
    TEST_ASSERT(ucn_cluster_get_view(&node->cluster, &view) == UCN_OK);
    TEST_ASSERT(view.parent_cluster_id == 0U && view.recovery_round == 0U);

    /* ---- (e) a detach before the reset period cancels the timer. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = now_ms;
    node = &network.nodes[2];
    node->cluster.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    node->cluster.pending_head_node_id = network.nodes[0].node_id;
    node->cluster.pending_cluster_id = 9U;
    node->cluster.pending_term = 7U;
    node->cluster.pending_join_nonce = 1U;
    node->cluster.parent_cluster_id = 4U;
    node->cluster.parent_term = 2U;
    node->cluster.recovery_round = 6U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_JOIN_PENDING;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_JOIN_INITIATED;
    node->cluster.shadow_transition_count = 0U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster,
                                    network.nodes[0].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(node->cluster.lineage_reset_deadline_ms != 0U);
    /* Ordered stepdown detaches before the reset period completes. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_STEPDOWN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 9U;
    message.term = 7U;
    message.head_node_id = network.nodes[0].node_id;
    message.head_score = 9000U;
    message.lease_ms = 8000U;
    message.nonce = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster,
                                    network.nodes[0].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(node->cluster.lineage_reset_deadline_ms == 0U); /* cancelled */
    TEST_ASSERT(node->cluster.parent_cluster_id == 4U); /* lineage kept */

    /* ---- (f) a recovery-domain join never arms the reset. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = now_ms;
    node = &network.nodes[2];
    node->cluster.role = UCN_CLUSTER_ROLE_DETACHED;
    node->cluster.recovery_eligible = true;
    node->cluster.parent_cluster_id = 9U;
    node->cluster.parent_term = 1U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_OBSERVE;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    node->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 55U;
    message.term = 1U;
    message.head_node_id = network.nodes[1].node_id;
    message.recovery_nonce = 7U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 9U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster,
                                    network.nodes[1].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(node->cluster.cluster_id == 55U);
    TEST_ASSERT(node->cluster.lineage_reset_deadline_ms == 0U); /* not armed */
    TEST_ASSERT(node->cluster.parent_cluster_id == 9U); /* lineage kept */
    return 0;
}

/* CLV2-M12 (12-04): deterministic Recovery rank.  Same parent ranks by
 * parent_term DESC, parent_config_id DESC, score DESC, node_id ASC; a
 * T9 island is never suppressed by a T8 island's higher score; different
 * parents are UNRANKABLE (no cross-parent term comparison, no recovery
 * cross-yield). */
static int cluster_test_recovery_rank(void)
{
    ucn_cluster_recovery_rank_t t9;
    ucn_cluster_recovery_rank_t t8_high_score;
    ucn_cluster_recovery_rank_t same_low_score;
    ucn_cluster_recovery_rank_t other_parent;
    ucn_cluster_recovery_rank_t unknown_parent;

    (void)memset(&t9, 0, sizeof(t9));
    t9.parent_cluster_id = 5U;
    t9.parent_term = 9U;
    t9.parent_config_id = 1U;
    t9.score = 100U;
    t9.node_id = 9U;

    t8_high_score = t9;
    t8_high_score.parent_term = 8U;
    t8_high_score.score = 9000U;
    t8_high_score.node_id = 1U;

    /* (1) T9 wins over T8 regardless of score/node. */
    TEST_ASSERT(ucn_cluster_recovery_rank_compare(&t9, &t8_high_score) ==
                UCN_CLUSTER_RECOVERY_RANK_A_WINS);
    TEST_ASSERT(ucn_cluster_recovery_rank_compare(&t8_high_score, &t9) ==
                UCN_CLUSTER_RECOVERY_RANK_B_WINS);

    /* (2) Same term/config: score DESC decides (t9.score=100 wins). */
    same_low_score = t9;
    same_low_score.score = 50U;
    TEST_ASSERT(ucn_cluster_recovery_rank_compare(&t9, &same_low_score) ==
                UCN_CLUSTER_RECOVERY_RANK_A_WINS);
    TEST_ASSERT(ucn_cluster_recovery_rank_compare(&same_low_score, &t9) ==
                UCN_CLUSTER_RECOVERY_RANK_B_WINS);

    /* (3) Same term/score: node_id ASC decides. */
    {
        ucn_cluster_recovery_rank_t high_node = t9;
        high_node.score = 500U;
        same_low_score = t9;
        same_low_score.score = 500U;
        same_low_score.node_id = 3U;
        TEST_ASSERT(ucn_cluster_recovery_rank_compare(&same_low_score,
                                                      &high_node) ==
                    UCN_CLUSTER_RECOVERY_RANK_A_WINS);
    }

    /* (4) config_id DESC beats a higher score. */
    {
        ucn_cluster_recovery_rank_t high_cfg = t9;
        ucn_cluster_recovery_rank_t low_cfg = t9;
        high_cfg.parent_config_id = 7U;
        high_cfg.score = 100U;
        low_cfg.parent_config_id = 3U;
        low_cfg.score = 9000U;
        TEST_ASSERT(ucn_cluster_recovery_rank_compare(&high_cfg, &low_cfg) ==
                    UCN_CLUSTER_RECOVERY_RANK_A_WINS);
    }

    /* (5) Different parents are UNRANKABLE in both directions. */
    other_parent = t9;
    other_parent.parent_cluster_id = 6U;
    TEST_ASSERT(ucn_cluster_recovery_rank_compare(&t9, &other_parent) ==
                UCN_CLUSTER_RECOVERY_RANK_UNRANKABLE);
    TEST_ASSERT(ucn_cluster_recovery_rank_compare(&other_parent, &t9) ==
                UCN_CLUSTER_RECOVERY_RANK_UNRANKABLE);

    /* (6) Identical inputs are EQUAL; a zero parent is never rankable. */
    TEST_ASSERT(ucn_cluster_recovery_rank_compare(&t9, &t9) ==
                UCN_CLUSTER_RECOVERY_RANK_EQUAL);
    (void)memset(&unknown_parent, 0, sizeof(unknown_parent));
    unknown_parent.parent_term = 9U;
    TEST_ASSERT(ucn_cluster_recovery_rank_compare(&unknown_parent, &t9) ==
                UCN_CLUSTER_RECOVERY_RANK_UNRANKABLE);
    return 0;
}

/* CLV2-M12 (12-04): the lineage-aware DECLARE arbitration.  A T8 island's
 * Recovery Head yields to a same-parent T9 contender; the reverse keeps
 * contending; a different-parent contender never cross-yields. */
static int cluster_test_recovery_rank_arbitration(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_node_id_t remote;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    head = &network.nodes[0];
    remote = network.nodes[1].node_id;
    head->cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    head->cluster.cluster_id = 51U;
    head->cluster.recovery_cluster_id = 51U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.recovery_nonce = 11U;
    head->cluster.parent_cluster_id = 5U;
    head->cluster.parent_term = 8U;
    head->cluster.term = 8U; /* mirrors the parent term */
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    head->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    head->cluster.shadow_transition_count = 0U;

    /* ---- (a) same-parent T9 contender: yield (rank loss). */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 52U;
    message.term = 9U;
    message.head_node_id = remote;
    message.recovery_nonce = 22U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, remote, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(head->cluster.cluster_id == 52U);
    TEST_ASSERT(head->cluster.term == 9U); /* adopted the T9 lineage term */
    TEST_ASSERT(head->cluster.shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(head->cluster.transition_reason ==
                UCN_CLUSTER_REASON_RECOVERY_YIELDED);

    /* ---- (b) reverse: a T9 head keeps contending against T8. */
    head = &network.nodes[0];
    head->cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    head->cluster.cluster_id = 61U;
    head->cluster.recovery_cluster_id = 61U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.recovery_nonce = 11U;
    head->cluster.parent_cluster_id = 5U;
    head->cluster.parent_term = 9U;
    head->cluster.term = 9U;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    head->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    head->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 62U;
    message.term = 8U;
    message.head_node_id = remote;
    message.recovery_nonce = 22U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, remote, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    TEST_ASSERT(head->cluster.cluster_id == 61U);
    TEST_ASSERT(head->cluster.shadow_transition_count == 0U);

    /* ---- (c) a different-parent contender never cross-yields. */
    message.cluster_id = 72U;
    message.term = 100U; /* huge, but a different parent: NOT comparable */
    message.recovery_parent_cluster_id = 6U;
    message.recovery_nonce = 23U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, remote, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    TEST_ASSERT(head->cluster.cluster_id == 61U);
    TEST_ASSERT(head->cluster.shadow_transition_count == 0U);
    return 0;
}

/* CLV2-M12 (12-10): the M12 recovery suite - the 12-01..09 mechanisms
 * composed end to end.  Safety-4 (islands of different parents never
 * merge) and Liveness-4/5 (headless domain converges; a returning stable
 * Head reclaims) are pinned by these tick-driven scenarios. */
static int cluster_test_recovery_suite_m12(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *a;
    cluster_test_node_t *b;
    uint32_t now_ms;
    uint32_t first_recovery_id;

    /* ---- (1) Primary+Backup both dead, two same-lineage survivors:
     * rank arbitration converges to a single Recovery Head. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.nodes[0].alive = false; /* Primary gone */
    network.nodes[1].alive = false; /* Backup gone */
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
    for (now_ms = 0U; now_ms <= 40U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    /* Both survivors captured the same lineage A/1 at their fence exit,
     * and the rank arbitration converged them into one recovery domain
     * (node 3 wins on node_id ASC for the same parent term). */
    TEST_ASSERT(a->cluster.parent_cluster_id == 1U);
    TEST_ASSERT(b->cluster.parent_cluster_id == 1U);
    TEST_ASSERT(a->cluster.role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    TEST_ASSERT(b->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(b->cluster.head_node_id == a->node_id);
    TEST_ASSERT(b->cluster.cluster_id == a->cluster.cluster_id);
    TEST_ASSERT(b->cluster.recovery_cluster_id == a->cluster.cluster_id);
    TEST_ASSERT(a->cluster.term == 1U); /* mirrors the parent term */
    first_recovery_id = a->cluster.cluster_id;
    TEST_ASSERT(first_recovery_id != 1U); /* never the parent ID */

    /* ---- (2) TTL cycles: round escalates across the fixture's 30 ms
     * TTL, and every new round derives a fresh Recovery ID (no identity
     * reuse - the full per-round uniqueness contract is pinned in the
     * 12-02 unit test). */
    for (now_ms = 41U; now_ms <= 200U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(a->cluster.recovery_round >= 1U);
    TEST_ASSERT(a->cluster.parent_cluster_id == 1U); /* lineage survives */
    if (a->cluster.role == UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
        /* A re-declared round always carries a fresh identity. */
        TEST_ASSERT(a->cluster.cluster_id != first_recovery_id);
        TEST_ASSERT(a->cluster.cluster_id != 1U);
    }

    /* ---- (3) Stable reclaim: the parent Head returns at A/2 and
     * outranks the recovery domain - Head stepdown + Member reclaim. */
    network.nodes[0].alive = true;
    network.nodes[0].cluster.role = UCN_CLUSTER_ROLE_HEAD;
    network.nodes[0].cluster.cluster_id = 1U;
    network.nodes[0].cluster.term = 2U;
    network.nodes[0].cluster.head_node_id = network.nodes[0].node_id;
    network.nodes[0].cluster.current_head_score = 9000U;
    network.nodes[0].cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    network.nodes[0].cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    network.nodes[0].cluster.shadow_transition_count = 0U;
    network.nodes[0].cluster.next_advertise_ms = 201U;
    for (now_ms = 201U; now_ms <= 350U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    /* The recovery survivors end up in the stable cluster A/2. */
    TEST_ASSERT(a->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(a->cluster.cluster_id == 1U);
    TEST_ASSERT(a->cluster.term == 2U);
    TEST_ASSERT(a->cluster.head_node_id == network.nodes[0].node_id);
    TEST_ASSERT(b->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(b->cluster.cluster_id == 1U);
    TEST_ASSERT(b->cluster.head_node_id == network.nodes[0].node_id);

    /* ---- (4) different-lineage islands never merge (Safety-4). */
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
    b->cluster.cluster_id = 2U; /* a DIFFERENT dead parent */
    b->cluster.term = 1U;
    b->cluster.head_node_id = UINT32_C(2);
    b->cluster.head_lease_expires_at_ms = 1U;
    for (now_ms = 0U; now_ms <= 40U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    /* Both stay their own island's Recovery Head: no cross-parent yield
     * (checked before the fixture's 30 ms TTL cycle lands). */
    TEST_ASSERT(a->cluster.parent_cluster_id == 1U);
    TEST_ASSERT(b->cluster.parent_cluster_id == 2U);
    TEST_ASSERT(a->cluster.role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    TEST_ASSERT(b->cluster.role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    TEST_ASSERT(a->cluster.cluster_id != b->cluster.cluster_id);
    return 0;
}

/* CLV2-M12 (12-08): min_recovery_peers isolation policy.  A plain member
 * needs max(default 1, configured) visible ADMITTED peers; a Backup with
 * a mirror keeps its distinct majority threshold; a fully isolated node
 * can never self-declare. */
static int cluster_test_recovery_isolation_policy(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;

    /* ---- (a) default policy: 0 visible peers -> no self-declare. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.nodes[1].alive = false;
    network.nodes[2].alive = false;
    network.nodes[3].alive = false;
    node = &network.nodes[0];
    node->cluster.config.head_capable = true;
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    node->cluster.recovery_eligible = true;
    TEST_ASSERT(cluster_test_sync_neighbors(&network) == 0);
    TEST_ASSERT(recovery_quorum_met(&node->cluster) == false);

    /* ---- (b) one visible peer satisfies the default threshold of 1. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.nodes[2].alive = false;
    network.nodes[3].alive = false;
    node = &network.nodes[0];
    node->cluster.config.head_capable = true;
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    node->cluster.recovery_eligible = true;
    TEST_ASSERT(cluster_test_sync_neighbors(&network) == 0);
    TEST_ASSERT(recovery_quorum_met(&node->cluster) == true);

    /* ---- (c) a raised bar: min_recovery_peers == 2 needs two peers. */
    node->cluster.config.min_recovery_peers = 2U;
    TEST_ASSERT(recovery_quorum_met(&node->cluster) == false);
    network.nodes[2].alive = true;
    TEST_ASSERT(cluster_test_sync_neighbors(&network) == 0);
    TEST_ASSERT(recovery_quorum_met(&node->cluster) == true);
    node->cluster.config.min_recovery_peers = 1U;

    /* ---- (d) a Backup mirror keeps its distinct majority threshold:
     * mirror of 3 protected voters requires 2 visible; one is not enough. */
    {
        ucn_cluster_member_t *slot;

        TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
        network.nodes[3].alive = false;
        node = &network.nodes[1];
        node->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
        node->cluster.recovery_eligible = true;
        node->cluster.backup_primary_node_id = network.nodes[0].node_id;
        slot = &node->cluster.primary_members.slots[0];
        (void)memset(slot, 0, sizeof(*slot));
        slot->occupied = true;
        slot->node_id = network.nodes[0].node_id;
        slot->voting = true;
        slot = &node->cluster.primary_members.slots[1];
        (void)memset(slot, 0, sizeof(*slot));
        slot->occupied = true;
        slot->node_id = node->node_id;
        slot->voting = true;
        slot = &node->cluster.primary_members.slots[2];
        (void)memset(slot, 0, sizeof(*slot));
        slot->occupied = true;
        slot->node_id = network.nodes[2].node_id;
        slot->voting = true;
        /* nodes 0,1,2 alive (3 dead): mirror voters 0/2 visible of {0,1,2}. */
        TEST_ASSERT(cluster_test_sync_neighbors(&network) == 0);
        TEST_ASSERT(primary_member_protected_voter_count_u16(&node->cluster) ==
                    3U);
        /* visible mirror voters: node 0 and node 2 are ADMITTED peers of
         * node 1; node 1 itself is not one of its own peers.  Only peers
         * 0 and 2 count -> 2 of 3 = majority met. */
        TEST_ASSERT(recovery_quorum_met(&node->cluster) == true);
        /* Drop peer 2: 1 of 3 visible -> majority NOT met. */
        network.nodes[2].alive = false;
        TEST_ASSERT(cluster_test_sync_neighbors(&network) == 0);
        TEST_ASSERT(recovery_quorum_met(&node->cluster) == false);
    }
    return 0;
}

/* CLV2-M12.1 (MAJOR-2): a Recovery Member that already follows a winner
 * compares a DIFFERENT-source candidate against the current accepted Head
 * rank, not against itself.  A delayed loser can never re-tear a converged
 * island. */
static int cluster_test_recovery_member_winner_fence(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *member;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_node_id_t winner;
    ucn_node_id_t loser;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    member = &network.nodes[2];
    winner = network.nodes[0].node_id; /* node 1: better on node ASC */
    loser = network.nodes[1].node_id;  /* node 2: worse */

    /* ---- (a) Member follows H1 (node 1), delayed H2 (node 2) arrives:
     * IGNORE (2 > 1 on node ASC). */
    member->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    member->cluster.cluster_id = 52U;
    member->cluster.recovery_cluster_id = 52U;
    member->cluster.term = 9U; /* mirrors the accepted Head's parent term */
    member->cluster.head_node_id = winner;
    member->cluster.parent_cluster_id = 5U;
    member->cluster.parent_term = 9U;
    member->cluster.accepted_recovery_nonce = 22U;
    member->cluster.known_recovery_source = winner;
    member->cluster.head_lease_expires_at_ms = 500U;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    member->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    member->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 62U;
    message.term = 9U;
    message.head_node_id = loser;
    message.recovery_nonce = 23U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, loser, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(member->cluster.cluster_id == 52U); /* still H1's domain */
    TEST_ASSERT(member->cluster.head_node_id == winner);
    TEST_ASSERT(member->cluster.accepted_recovery_nonce == 22U);
    TEST_ASSERT(member->cluster.shadow_transition_count == 0U);
    TEST_ASSERT(member->cluster.stats.stale_messages == 1U);

    /* ---- (b) Member follows the loser H2, then the better H1 arrives:
     * SWITCH. */
    member->cluster.cluster_id = 62U;
    member->cluster.recovery_cluster_id = 62U;
    member->cluster.head_node_id = loser;
    member->cluster.accepted_recovery_nonce = 23U;
    member->cluster.known_recovery_source = loser;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 52U;
    message.term = 9U;
    message.head_node_id = winner;
    message.recovery_nonce = 22U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, winner, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.cluster_id == 52U); /* switched to H1 */
    TEST_ASSERT(member->cluster.head_node_id == winner);
    TEST_ASSERT(member->cluster.known_recovery_source == winner);

    /* ---- (c) Member follows T9/H8, incoming T8/H1: IGNORE (term DESC
     * dominates node ASC). */
    member->cluster.cluster_id = 71U;
    member->cluster.recovery_cluster_id = 71U;
    member->cluster.term = 9U;
    member->cluster.head_node_id = loser; /* stands in for H8 */
    member->cluster.known_recovery_source = loser;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 72U;
    message.term = 8U; /* an OLDER parent generation */
    message.head_node_id = winner; /* lower node id does not help */
    message.recovery_nonce = 24U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, winner, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(member->cluster.cluster_id == 71U);
    TEST_ASSERT(member->cluster.head_node_id == loser);

    /* ---- (d) Member follows T8/H1, incoming T9/H8: SWITCH (the newer
     * parent generation wins even with a worse node id). */
    member->cluster.term = 8U;
    member->cluster.cluster_id = 81U;
    member->cluster.recovery_cluster_id = 81U;
    member->cluster.head_node_id = winner;
    member->cluster.known_recovery_source = winner;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 82U;
    message.term = 9U;
    message.head_node_id = loser;
    message.recovery_nonce = 25U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, loser, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.cluster_id == 82U);
    TEST_ASSERT(member->cluster.head_node_id == loser);
    TEST_ASSERT(member->cluster.term == 9U);
    return 0;
}

/* CLV2-M12.2: accepting a lineage-aware Recovery Head must teach a
 * parentless/lagging Member the parent ID/Term before a delayed loser can
 * exploit the formerly empty local lineage domain.  It also pins exact Term
 * binding for both DECLARE lease refresh and ACK admission. */
static int cluster_test_recovery_lineage_adoption_and_term_binding(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *member;
    cluster_test_node_t *head;
    ucn_cluster_message_t message;
    ucn_cluster_t before;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    uint32_t lease_before;
    ucn_node_id_t winner;
    ucn_node_id_t loser;

    /* (a) A parentless late survivor joins H1/A/T9, adopts A/T9, then
     * rejects delayed H2/A/T9 because current-winner fencing is now live. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    member = &network.nodes[2];
    winner = network.nodes[0].node_id;
    loser = network.nodes[1].node_id;
    member->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    member->cluster.cluster_id = 0U;
    member->cluster.recovery_cluster_id = 0U;
    member->cluster.term = 0U;
    member->cluster.head_node_id = 0U;
    member->cluster.parent_cluster_id = 0U;
    member->cluster.parent_term = 0U;
    member->cluster.parent_config_id = 44U; /* v3 must not overwrite this. */
    member->cluster.recovery_round = 7U; /* DECLARE must not reset it. */
    member->cluster.recovery_nonce = 0U;
    member->cluster.accepted_recovery_nonce = 0U;
    member->cluster.known_recovery_source = 0U;
    /* An expired non-zero lease is the canonical on-wire representation of
     * a survivor that has lost its old Head.  Zero means "no lease armed"
     * and remains intentionally fail-closed in the Member RX guard. */
    member->cluster.head_lease_expires_at_ms = 50U;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    member->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    member->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 52U;
    message.term = 9U;
    message.head_node_id = winner;
    message.recovery_nonce = 22U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, winner, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.cluster_id == 52U &&
                member->cluster.recovery_cluster_id == 52U &&
                member->cluster.term == 9U &&
                member->cluster.head_node_id == winner &&
                member->cluster.parent_cluster_id == 5U &&
                member->cluster.parent_term == 9U &&
                member->cluster.parent_config_id == 44U &&
                member->cluster.recovery_round == 7U);
    message.cluster_id = 62U;
    message.head_node_id = loser;
    message.recovery_nonce = 23U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, loser, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(member->cluster.cluster_id == 52U &&
                member->cluster.head_node_id == winner &&
                member->cluster.parent_cluster_id == 5U &&
                member->cluster.parent_term == 9U &&
                member->cluster.recovery_round == 7U);

    /* (b) A higher same-parent Term is forward-adopted and remains so when
     * a later recovery-domain lineage capture sees older stable history. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    member = &network.nodes[2];
    winner = network.nodes[0].node_id;
    loser = network.nodes[1].node_id;
    member->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    member->cluster.cluster_id = 71U;
    member->cluster.recovery_cluster_id = 71U;
    member->cluster.term = 8U;
    member->cluster.head_node_id = winner;
    member->cluster.parent_cluster_id = 5U;
    member->cluster.parent_term = 8U;
    member->cluster.parent_config_id = 44U;
    member->cluster.last_cluster_id = 5U;
    member->cluster.max_seen_term = 8U;
    member->cluster.accepted_recovery_nonce = 22U;
    member->cluster.known_recovery_source = winner;
    member->cluster.head_lease_expires_at_ms = 500U;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    member->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    member->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 72U;
    message.term = 9U;
    message.head_node_id = loser;
    message.recovery_nonce = 23U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, loser, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.cluster_id == 72U &&
                member->cluster.term == 9U &&
                member->cluster.head_node_id == loser &&
                member->cluster.parent_cluster_id == 5U &&
                member->cluster.parent_term == 9U &&
                member->cluster.parent_config_id == 44U);
    ucn_cluster_test_lineage_capture(&member->cluster);
    TEST_ASSERT(member->cluster.parent_term == 9U);

    /* (c) Same source/cluster/nonce/parent with a wrong Term cannot extend
     * a Member lease. */
    lease_before = member->cluster.head_lease_expires_at_ms;
    before = member->cluster;
    message.term = 8U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, loser, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(member->cluster.head_lease_expires_at_ms == lease_before &&
                member->cluster.term == 9U &&
                member->cluster.parent_term == 9U);
    before.stats.messages_received++;
    before.stats.stale_messages++;
    TEST_ASSERT(memcmp(&member->cluster, &before, sizeof(before)) == 0);

    /* (d) Recovery Head rejects an ACK with the wrong Term before member
     * allocation/ack-count/lease effects, while the exact ACK still works. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    head = &network.nodes[0];
    member = &network.nodes[2];
    head->cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    head->cluster.cluster_id = 82U;
    head->cluster.recovery_cluster_id = 82U;
    head->cluster.term = 9U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.recovery_nonce = 22U;
    head->cluster.parent_cluster_id = 5U;
    head->cluster.parent_term = 9U;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    head->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    head->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_ACK;
    message.role = UCN_CLUSTER_ROLE_MEMBER;
    message.cluster_id = 82U;
    message.term = 8U;
    message.head_node_id = head->node_id;
    message.recovery_nonce = 22U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    before = head->cluster;
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, member->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(head->cluster.recovery_ack_count == 0U &&
                primary_member_find(&head->cluster, member->node_id) == NULL);
    before.stats.messages_received++;
    before.stats.stale_messages++;
    TEST_ASSERT(memcmp(&head->cluster, &before, sizeof(before)) == 0);
    message.term = 9U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, member->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.recovery_ack_count == 1U &&
                primary_member_find(&head->cluster, member->node_id) != NULL);
    return 0;
}

/* CLV2-M12.3 independent full-audit closure.  These cases cross the
 * 12-03/06/07 boundaries and deliberately fail the pre-M12.3 code:
 * - stable JOIN must scrub the old Recovery identity;
 * - a stable lineage-reset timer must not survive a Recovery join;
 * - one Recovery ID cannot be rebound to another nonce/Head;
 * - legacy current-winner arbitration compares against the accepted Head;
 * - known lineage fences an idle survivor before it starts contending;
 * - ACK role/round is structurally exact;
 * - a live Stable Backup cannot be peeled into a Recovery domain. */
static int cluster_test_recovery_domain_exit_and_exact_identity(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *member;
    cluster_test_node_t *stable;
    cluster_test_node_t *recovery;
    ucn_cluster_message_t message;
    ucn_cluster_t before;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    uint32_t lease_before;

    /* (a) A Recovery Member completes stable JOIN_ACCEPT.  Every old
     * Recovery identity field is scrubbed before the Stable Epoch is
     * installed, and the delayed old DECLARE cannot refresh its lease. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    stable = &network.nodes[0];
    recovery = &network.nodes[1];
    member = &network.nodes[2];
    member->cluster.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    member->cluster.cluster_id = 52U;
    member->cluster.term = 9U;
    member->cluster.head_node_id = recovery->node_id;
    member->cluster.pending_head_node_id = stable->node_id;
    member->cluster.pending_cluster_id = 5U;
    member->cluster.pending_term = 9U;
    member->cluster.pending_join_nonce = 77U;
    member->cluster.recovery_cluster_id = 52U;
    member->cluster.recovery_deadline_ms = 150U;
    member->cluster.recovery_cooldown_until_ms = 160U;
    member->cluster.recovery_backoff_deadline_ms = 170U;
    member->cluster.recovery_nonce = 22U;
    member->cluster.accepted_recovery_nonce = 22U;
    member->cluster.known_recovery_source = recovery->node_id;
    member->cluster.recovery_ack_count = 1U;
    member->cluster.recovery_acked = 1U;
    member->cluster.parent_cluster_id = 5U;
    member->cluster.parent_term = 9U;
    member->cluster.recovery_round = 4U;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_JOIN_PENDING;
    member->cluster.transition_reason = UCN_CLUSTER_REASON_STABLE_RECLAIM;
    member->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_JOIN_ACCEPT;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 5U;
    message.term = 9U;
    message.head_node_id = stable->node_id;
    message.head_score = 4000U;
    message.available_capacity = 4U;
    message.lease_ms = 8000U;
    message.nonce = 77U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, stable->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.role == UCN_CLUSTER_ROLE_MEMBER &&
                member->cluster.cluster_id == 5U &&
                member->cluster.term == 9U &&
                member->cluster.head_node_id == stable->node_id);
    TEST_ASSERT(member->cluster.recovery_cluster_id == 0U &&
                member->cluster.recovery_deadline_ms == 0U &&
                member->cluster.recovery_cooldown_until_ms == 0U &&
                member->cluster.recovery_backoff_deadline_ms == 0U &&
                member->cluster.recovery_nonce == 0U &&
                member->cluster.accepted_recovery_nonce == 0U &&
                member->cluster.known_recovery_source == 0U &&
                member->cluster.recovery_ack_count == 0U &&
                member->cluster.recovery_acked == 0U &&
                member->cluster.recovery_round == 4U &&
                member->cluster.lineage_reset_deadline_ms != 0U);
    lease_before = member->cluster.head_lease_expires_at_ms;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 52U;
    message.term = 9U;
    message.head_node_id = recovery->node_id;
    message.recovery_nonce = 22U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, recovery->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_ACCESS);
    TEST_ASSERT(member->cluster.cluster_id == 5U &&
                member->cluster.head_node_id == stable->node_id &&
                member->cluster.head_lease_expires_at_ms == lease_before &&
                member->cluster.recovery_cluster_id == 0U);

    /* (b) A direct lease-expired switch from Stable Member to Recovery
     * cancels the previously armed stable lineage-reset timer. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    stable = &network.nodes[0];
    recovery = &network.nodes[1];
    member = &network.nodes[2];
    member->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    member->cluster.cluster_id = 5U;
    member->cluster.term = 9U;
    member->cluster.head_node_id = stable->node_id;
    member->cluster.head_lease_expires_at_ms = 50U;
    member->cluster.parent_cluster_id = 5U;
    member->cluster.parent_term = 9U;
    member->cluster.recovery_round = 4U;
    member->cluster.lineage_reset_deadline_ms = 150U;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    member->cluster.transition_reason = UCN_CLUSTER_REASON_JOIN_ACCEPTED;
    member->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 62U;
    message.term = 9U;
    message.head_node_id = recovery->node_id;
    message.recovery_nonce = 33U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, recovery->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(ucn_cluster_recovery_scoped(&member->cluster) &&
                member->cluster.lineage_reset_deadline_ms == 0U &&
                member->cluster.parent_cluster_id == 5U &&
                member->cluster.parent_term == 9U &&
                member->cluster.recovery_round == 4U);
    network.now_ms = 200U;
    TEST_ASSERT(ucn_cluster_step(&member->cluster) == UCN_OK);
    TEST_ASSERT(member->cluster.parent_cluster_id == 5U &&
                member->cluster.parent_term == 9U &&
                member->cluster.recovery_round == 4U);

    /* (c) One Recovery ID is one exact identity.  Same-source nonce reuse
     * and a second Head collision both reject with only RX/stale stats. */
    network.now_ms = 201U;
    before = member->cluster;
    message.recovery_nonce = 34U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, recovery->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    before.stats.messages_received++;
    before.stats.stale_messages++;
    TEST_ASSERT(memcmp(&member->cluster, &before, sizeof(before)) == 0);
    before = member->cluster;
    message.head_node_id = stable->node_id;
    message.recovery_nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, stable->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    before.stats.messages_received++;
    before.stats.stale_messages++;
    TEST_ASSERT(memcmp(&member->cluster, &before, sizeof(before)) == 0);

    /* (d) Unknown-lineage compatibility still converges deterministically:
     * a worse tuple cannot tear the current winner; a better tuple can. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    stable = &network.nodes[0];  /* current legacy winner */
    recovery = &network.nodes[1];
    member = &network.nodes[2];
    member->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    member->cluster.cluster_id = 52U;
    member->cluster.recovery_cluster_id = 52U;
    member->cluster.term = 1U;
    member->cluster.head_node_id = stable->node_id;
    member->cluster.accepted_recovery_nonce = 10U;
    member->cluster.known_recovery_source = stable->node_id;
    member->cluster.head_lease_expires_at_ms = 500U;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    member->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 62U;
    message.term = 1U;
    message.head_node_id = recovery->node_id;
    message.recovery_nonce = 11U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 0U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, recovery->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(member->cluster.cluster_id == 52U &&
                member->cluster.head_node_id == stable->node_id);
    message.recovery_nonce = 9U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, recovery->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.cluster_id == 62U &&
                member->cluster.head_node_id == recovery->node_id &&
                member->cluster.accepted_recovery_nonce == 9U);

    /* (e) A headless-but-lineaged survivor has not minted a local nonce yet.
     * It still rejects an older same-parent Term and an unknown-parent
     * downgrade; an exact current-parent candidate remains joinable. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    recovery = &network.nodes[1];
    member = &network.nodes[2];
    member->cluster.role = UCN_CLUSTER_ROLE_DETACHED;
    member->cluster.recovery_eligible = true;
    member->cluster.recovery_nonce = 0U;
    member->cluster.parent_cluster_id = 5U;
    member->cluster.parent_term = 9U;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_OBSERVE;
    member->cluster.transition_reason = UCN_CLUSTER_REASON_GRACE_TIMEOUT;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 72U;
    message.term = 8U;
    message.head_node_id = recovery->node_id;
    message.recovery_nonce = 20U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    before = member->cluster;
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, recovery->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    before.stats.messages_received++;
    before.stats.stale_messages++;
    TEST_ASSERT(memcmp(&member->cluster, &before, sizeof(before)) == 0);

    message.cluster_id = 73U;
    message.term = 1U;
    message.recovery_nonce = 21U;
    message.recovery_parent_cluster_id = 0U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    before = member->cluster;
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, recovery->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    before.stats.messages_received++;
    before.stats.stale_messages++;
    TEST_ASSERT(memcmp(&member->cluster, &before, sizeof(before)) == 0);

    message.cluster_id = 74U;
    message.term = 9U;
    message.recovery_nonce = 22U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, recovery->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.role == UCN_CLUSTER_ROLE_MEMBER &&
                member->cluster.cluster_id == 74U &&
                member->cluster.parent_cluster_id == 5U &&
                member->cluster.parent_term == 9U);

    /* (f) ACK must carry MEMBER role and a non-zero exact round at the
     * structural codec boundary; malformed variants allocate no member. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    recovery = &network.nodes[0];
    member = &network.nodes[2];
    recovery->cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    recovery->cluster.cluster_id = 82U;
    recovery->cluster.recovery_cluster_id = 82U;
    recovery->cluster.term = 9U;
    recovery->cluster.head_node_id = recovery->node_id;
    recovery->cluster.recovery_nonce = 22U;
    recovery->cluster.parent_cluster_id = 5U;
    recovery->cluster.parent_term = 9U;
    recovery->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    recovery->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_ACK;
    message.role = UCN_CLUSTER_ROLE_MEMBER;
    message.cluster_id = 82U;
    message.term = 9U;
    message.head_node_id = recovery->node_id;
    message.recovery_nonce = 22U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    encoded[2U] = (uint8_t)UCN_CLUSTER_ROLE_HEAD;
    TEST_ASSERT(ucn_cluster_receive(&recovery->cluster, member->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_MALFORMED);
    TEST_ASSERT(primary_member_find(&recovery->cluster, member->node_id) ==
                NULL);
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    (void)memset(encoded + 16U, 0, 4U); /* recovery_nonce = 0 */
    TEST_ASSERT(ucn_cluster_receive(&recovery->cluster, member->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_MALFORMED);
    TEST_ASSERT(primary_member_find(&recovery->cluster, member->node_id) ==
                NULL);

    /* (g) Stable precedence applies to Backup as well as Member.  While
     * the Primary lease is live, an otherwise valid Recovery declaration
     * is rejected before changing Backup identity, mirror or deadlines. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    stable = &network.nodes[0];
    recovery = &network.nodes[1];
    member = &network.nodes[2];
    member->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
    member->cluster.cluster_id = 5U;
    member->cluster.term = 9U;
    member->cluster.head_node_id = stable->node_id;
    member->cluster.backup_primary_node_id = stable->node_id;
    member->cluster.backup_primary_lease_deadline_ms = 500U;
    member->cluster.backup_ready = true;
    member->cluster.backup_syncing = false;
    member->cluster.backup_takeover_active = false;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 92U;
    message.term = 9U;
    message.head_node_id = recovery->node_id;
    message.recovery_nonce = 31U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    before = member->cluster;
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, recovery->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_ACCESS);
    before.stats.messages_received++;
    TEST_ASSERT(memcmp(&member->cluster, &before, sizeof(before)) == 0);
    return 0;
}

/* CLV2-M12 (12-06): RECOVERY_DECLARE/ACK round binding.  Old-round
 * declares from the current Head are REPLAY for a recovery member; the
 * next round is followed; the Head rejects old-round ACKs (REPLAY) and
 * different-parent ACKs (ACCESS) with zero writes; duplicate same-round
 * ACKs stay idempotent. */
static int cluster_test_recovery_round_binding(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    cluster_test_node_t *member;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_node_id_t remote;

    /* ---- (a/b) member side: old round REPLAY, next round followed. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    head = &network.nodes[0];
    member = &network.nodes[2];
    remote = head->node_id;
    member->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    member->cluster.cluster_id = 52U;
    member->cluster.recovery_cluster_id = 52U;
    member->cluster.term = 9U;
    member->cluster.head_node_id = remote;
    member->cluster.parent_cluster_id = 5U;
    member->cluster.parent_term = 9U;
    member->cluster.accepted_recovery_nonce = 22U;
    member->cluster.known_recovery_source = remote;
    member->cluster.head_lease_expires_at_ms = 500U;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    member->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    member->cluster.shadow_transition_count = 0U;

    /* Old round (same source, smaller nonce): REPLAY, zero writes. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = 51U;
    message.term = 9U;
    message.head_node_id = remote;
    message.recovery_nonce = 21U;
    message.recovery_ttl_ms = 30000U;
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, remote, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(member->cluster.cluster_id == 52U); /* current round kept */
    TEST_ASSERT(member->cluster.accepted_recovery_nonce == 22U);
    TEST_ASSERT(member->cluster.shadow_transition_count == 0U);
    TEST_ASSERT(member->cluster.stats.stale_messages == 1U);

    /* Next round (same source, larger nonce): followed via the join path. */
    message.cluster_id = 62U;
    message.recovery_nonce = 23U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, remote, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(member->cluster.cluster_id == 62U); /* adopted the new round */
    TEST_ASSERT(member->cluster.accepted_recovery_nonce == 23U);

    /* ---- (c/d/e) head side: old-round ACK REPLAY, wrong parent ACCESS,
     * duplicate ACK idempotent. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    head = &network.nodes[0];
    remote = network.nodes[2].node_id;
    head->cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    head->cluster.cluster_id = 52U;
    head->cluster.recovery_cluster_id = 52U;
    head->cluster.term = 9U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.recovery_nonce = 22U;
    head->cluster.parent_cluster_id = 5U;
    head->cluster.parent_term = 9U;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    head->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    head->cluster.shadow_transition_count = 0U;

    /* Old-round ACK: REPLAY, no member slot, no lease write. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_ACK;
    message.role = UCN_CLUSTER_ROLE_MEMBER;
    message.cluster_id = 52U;
    message.term = 9U;
    message.head_node_id = head->node_id;
    message.recovery_nonce = 21U; /* old round */
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, remote, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(head->cluster.recovery_ack_count == 0U);
    TEST_ASSERT(primary_member_find(&head->cluster, remote) == NULL);
    TEST_ASSERT(head->cluster.stats.stale_messages == 1U);

    /* Wrong-parent ACK: ACCESS, zero writes. */
    message.recovery_nonce = 22U;
    message.recovery_parent_cluster_id = 6U; /* different parent island */
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, remote, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_ACCESS);
    TEST_ASSERT(primary_member_find(&head->cluster, remote) == NULL);

    /* Correct current-round ACK: member admitted once. */
    message.recovery_parent_cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, remote, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.recovery_ack_count == 1U);
    {
        const ucn_cluster_member_t *admitted =
            primary_member_find(&head->cluster, remote);
        TEST_ASSERT(admitted != NULL);
        TEST_ASSERT(admitted->lease_expires_at_ms != 0U);
        /* Duplicate same-round ACK: idempotent lease refresh only. */
        TEST_ASSERT(ucn_cluster_receive(&head->cluster, remote, true,
                                        encoded, sizeof(encoded)) == UCN_OK);
        TEST_ASSERT(head->cluster.recovery_ack_count == 1U);
    }
    /* Recovery membership is scoped to this exact round.  TTL stepdown
     * clears the member proof; a later Recovery ID must collect a fresh ACK. */
    head->cluster.recovery_deadline_ms = 99U;
    TEST_ASSERT(ucn_cluster_step(&head->cluster) == UCN_OK);
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(primary_member_find(&head->cluster, remote) == NULL);
    TEST_ASSERT(head->cluster.recovery_ack_count == 0U);
    return 0;
}

/* CLV2-M12 (12-07): stable precedence.  A recovery-domain Member
 * reclaims to a legal stable Head of its parent lineage via JOIN_PENDING
 * (STABLE_RECLAIM) without any score/capacity gate; foreign stable Heads
 * stay excluded; the RECOVERY_HEAD ordered stepdown is preserved. */
static int cluster_test_recovery_stable_precedence(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *member;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_node_id_t stable_head;
    ucn_node_id_t recovery_head;

    /* ---- (a) member reclaim to a newer parent-lineage stable Head. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    member = &network.nodes[2];
    stable_head = network.nodes[0].node_id;
    recovery_head = network.nodes[1].node_id;
    member->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    member->cluster.cluster_id = 52U; /* recovery domain */
    member->cluster.recovery_cluster_id = 52U;
    member->cluster.term = 9U;
    member->cluster.head_node_id = recovery_head;
    member->cluster.parent_cluster_id = 5U;
    member->cluster.parent_term = 9U;
    member->cluster.current_head_score = 500U;
    member->cluster.head_lease_expires_at_ms = 5000U;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    member->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    member->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 5U; /* the parent lineage */
    message.term = 10U;
    message.head_node_id = stable_head;
    message.head_score = 100U; /* deliberately terrible: no score gate */
    message.available_capacity = 0U; /* full: no capacity gate either */
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, stable_head, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(member->cluster.shadow_phase == UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(member->cluster.transition_reason ==
                UCN_CLUSTER_REASON_STABLE_RECLAIM);
    TEST_ASSERT(member->cluster.pending_cluster_id == 5U);
    TEST_ASSERT(member->cluster.pending_term == 10U);
    TEST_ASSERT(member->cluster.pending_head_node_id == stable_head);
    TEST_ASSERT(member->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(member->cluster.stats.head_switches == 0U);

    /* ---- (b) a foreign stable Head stays excluded (11-08 freeze). */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    member = &network.nodes[2];
    stable_head = network.nodes[0].node_id;
    recovery_head = network.nodes[1].node_id;
    member->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    member->cluster.cluster_id = 52U;
    member->cluster.recovery_cluster_id = 52U;
    member->cluster.term = 9U;
    member->cluster.head_node_id = recovery_head;
    member->cluster.parent_cluster_id = 5U;
    member->cluster.parent_term = 9U;
    member->cluster.current_head_score = 500U;
    member->cluster.head_lease_expires_at_ms = 5000U;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    member->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    member->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 8U; /* foreign stable cluster */
    message.term = 100U;
    message.head_node_id = stable_head;
    message.head_score = 9000U;
    message.available_capacity = 4U;
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, stable_head, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(member->cluster.cluster_id == 52U);
    TEST_ASSERT(member->cluster.pending_cluster_id == 0U);
    TEST_ASSERT(member->cluster.shadow_transition_count == 0U);

    /* ---- (c) same-term parent offer also reclaims (term >= parent_term);
     * ---- (d) a RECOVERY_HEAD keeps its ordered stepdown on any stable
     * Head offer. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    member = &network.nodes[0];
    stable_head = network.nodes[1].node_id;
    member->cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    member->cluster.cluster_id = 61U;
    member->cluster.recovery_cluster_id = 61U;
    member->cluster.term = 9U;
    member->cluster.head_node_id = member->node_id;
    member->cluster.parent_cluster_id = 5U;
    member->cluster.parent_term = 9U;
    member->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    member->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    member->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 5U;
    message.term = 9U; /* same term: the domain is still outranked */
    message.head_node_id = stable_head;
    message.head_score = 9000U;
    message.available_capacity = 4U;
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, stable_head, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.role == UCN_CLUSTER_ROLE_STEPPING_DOWN);
    TEST_ASSERT(member->cluster.shadow_phase == UCN_CLUSTER_PHASE_STEPPING_DOWN);
    TEST_ASSERT(member->cluster.shadow_transition_count == 1U);
    return 0;
}

/* CLV2-M12 (12-05): recovery scope predicate + the v3 fail-closed pins
 * (a recovery domain can never run a takeover/config path against the
 * parent identity; receive-side historical takeover stays gated by the
 * R02/R03 proofs). */
static int cluster_test_recovery_scope(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    node = &network.nodes[0];

    /* (1) predicate truth table. */
    node->cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    node->cluster.cluster_id = 77U;
    node->cluster.recovery_cluster_id = 77U;
    TEST_ASSERT(ucn_cluster_recovery_scoped(&node->cluster) == true);
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    TEST_ASSERT(ucn_cluster_recovery_scoped(&node->cluster) == true);
    node->cluster.recovery_cluster_id = 0U; /* domain dissolved */
    node->cluster.cluster_id = 5U;
    TEST_ASSERT(ucn_cluster_recovery_scoped(&node->cluster) == false);
    node->cluster.role = UCN_CLUSTER_ROLE_DETACHED;
    node->cluster.cluster_id = 0U;
    TEST_ASSERT(ucn_cluster_recovery_scoped(&node->cluster) == false);
    TEST_ASSERT(ucn_cluster_recovery_scoped(NULL) == false);

    /* (2) a recovery-domain member rejects a BACKUP_ASSIGN claiming the
     * PARENT cluster identity (R01 epoch fence): zero writes. */
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    node->cluster.cluster_id = 77U;
    node->cluster.recovery_cluster_id = 77U;
    node->cluster.term = 9U;
    node->cluster.head_node_id = network.nodes[1].node_id;
    node->cluster.parent_cluster_id = 5U;
    node->cluster.parent_term = 9U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    node->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_ASSIGN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 5U; /* the PARENT identity, not the recovery ID */
    message.term = 9U;
    message.head_node_id = network.nodes[2].node_id;
    message.backup_generation = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster,
                                    network.nodes[2].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_ERR_ACCESS);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(node->cluster.cluster_id == 77U);
    TEST_ASSERT(node->cluster.backup_primary_node_id == 0U);
    TEST_ASSERT(node->cluster.shadow_transition_count == 0U);

    /* (3) a RECOVERY_HEAD only accepts a HEAD_TAKEOVER that passes the
     * R02/R03 historical proofs (parent lineage + max_seen_term + sender
     * equals the announced Head).  A forged sender is rejected. */
    node = &network.nodes[0];
    node->cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    node->cluster.cluster_id = 77U;
    node->cluster.recovery_cluster_id = 77U;
    node->cluster.term = 9U;
    node->cluster.head_node_id = node->node_id;
    node->cluster.parent_cluster_id = 5U;
    node->cluster.parent_term = 9U;
    node->cluster.max_seen_term = 9U;
    node->cluster.last_cluster_id = 5U;
    node->cluster.last_stable_head = network.nodes[1].node_id;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_WIN;
    node->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_TAKEOVER;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 5U; /* parent lineage: a legitimate reclaim target */
    message.term = 10U;
    message.head_node_id = network.nodes[2].node_id; /* NOT last_stable_head */
    message.head_score = 9000U;
    message.lease_ms = 8000U;
    message.backup_generation = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster,
                                    network.nodes[2].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_ERR_ACCESS);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    TEST_ASSERT(node->cluster.shadow_transition_count == 0U);
    return 0;
}

/* CLV2-01-04f (f3, SITE A): consider_head_offer() RECOVERY_* -> JOIN_PENDING
 * wiring.  A RECOVERY_OBSERVE / RECOVERY_ELECTION node (role DETACHED +
 * recovery_eligible; the armed backoff decides the sub-phase) accepting a
 * stable-Head offer commits RECOVERY_* -> JOIN_PENDING (JOIN_INITIATED)
 * through the single entry point UNCONDITIONALLY (the legacy event - the
 * stable Head offer - decides THAT the join runs; cluster_transition()
 * validates whether the shadow agrees), BEFORE any site write (apply_legacy
 * owns the role write), then the begin_join() field payload follows at the
 * site via begin_join_prepare_fields() + the post-commit derive assert.
 *
 *  (a) RECOVERY_OBSERVE source (no backoff): explicit transition, shadow ==
 *      JOIN_PENDING, reason == JOIN_INITIATED, count == 1, join payload
 *      applied (pending_* + role_since/next_join_retry), role JOIN_PENDING.
 *  (b) RECOVERY_ELECTION source (backoff armed): the same explicit
 *      transition; the armed backoff is cleared by apply_legacy(JOIN_PENDING)
 *      and the site (idempotent).
 *  (c) shadow-desync: a stale shadow fails closed with UCN_ERR_STATE, ZERO
 *      site writes and no join (pending_* untouched) - driven via the test
 *      hook so no end-of-RX sync can re-align the mirror afterwards. */
static int cluster_test_recovery_offer_join_wiring(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_candidate_t candidate;
    ucn_result_t result;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 100U;
    node = &network.nodes[2];
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.head_node_id = node->node_id + 1U; /* a stable foreign Head */
    candidate.cluster_id = 1U;
    candidate.term = 2U;
    candidate.head_score = 9000U;
    candidate.available_capacity = 3U;

    /* ============ (a) RECOVERY_OBSERVE + stable-Head offer ============ */
    node->cluster.role = UCN_CLUSTER_ROLE_DETACHED;
    node->cluster.recovery_eligible = true;
    node->cluster.recovery_backoff_deadline_ms = 0U;
    node->cluster.recovery_cooldown_until_ms = 0U;
    node->cluster.role_since_ms = 0U;
    node->cluster.next_join_retry_ms = 0U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_OBSERVE;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_RECOVERY_TTL_EXPIRED;
    node->cluster.shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(&node->cluster, 100U) ==
                UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    result = ucn_cluster_test_consider_head_offer(&node->cluster, &candidate,
                                                  100U);
    TEST_ASSERT(result == UCN_OK);
    TEST_ASSERT(node->cluster.shadow_phase == UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(node->cluster.transition_reason ==
                UCN_CLUSTER_REASON_JOIN_INITIATED);
    TEST_ASSERT(node->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(node->cluster.recovery_eligible == false);
    TEST_ASSERT(node->cluster.recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(node->cluster.pending_head_node_id == candidate.head_node_id);
    TEST_ASSERT(node->cluster.pending_cluster_id == 1U);
    TEST_ASSERT(node->cluster.pending_term == 2U);
    TEST_ASSERT(node->cluster.pending_head_score == 9000U);
    TEST_ASSERT(node->cluster.role_since_ms == 100U);
    TEST_ASSERT(node->cluster.next_join_retry_ms == 100U);
    TEST_ASSERT(test_derive_phase(&node->cluster, 100U) ==
                UCN_CLUSTER_PHASE_JOIN_PENDING);

    /* ============ (b) RECOVERY_ELECTION source (backoff armed) ============ */
    node->cluster.role = UCN_CLUSTER_ROLE_DETACHED;
    node->cluster.recovery_eligible = true;
    node->cluster.recovery_backoff_deadline_ms = 1U; /* armed backoff */
    node->cluster.recovery_cooldown_until_ms = 0U;
    node->cluster.role_since_ms = 0U;
    node->cluster.next_join_retry_ms = 0U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_ELECTION;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    node->cluster.shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(&node->cluster, 100U) ==
                UCN_CLUSTER_PHASE_RECOVERY_ELECTION);
    result = ucn_cluster_test_consider_head_offer(&node->cluster, &candidate,
                                                  100U);
    TEST_ASSERT(result == UCN_OK);
    TEST_ASSERT(node->cluster.shadow_phase == UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(node->cluster.transition_reason ==
                UCN_CLUSTER_REASON_JOIN_INITIATED);
    TEST_ASSERT(node->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(node->cluster.recovery_eligible == false);
    TEST_ASSERT(node->cluster.recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(node->cluster.pending_head_node_id == candidate.head_node_id);
    TEST_ASSERT(node->cluster.pending_cluster_id == 1U);
    TEST_ASSERT(node->cluster.pending_term == 2U);
    TEST_ASSERT(node->cluster.pending_head_score == 9000U);
    TEST_ASSERT(node->cluster.role_since_ms == 100U);
    TEST_ASSERT(node->cluster.next_join_retry_ms == 100U);
    TEST_ASSERT(test_derive_phase(&node->cluster, 100U) ==
                UCN_CLUSTER_PHASE_JOIN_PENDING);

    /* ============ (c) shadow-desync: fail closed, ZERO site writes ============ */
    node->cluster.role = UCN_CLUSTER_ROLE_DETACHED;
    node->cluster.recovery_eligible = true;
    node->cluster.recovery_backoff_deadline_ms = 0U;
    node->cluster.recovery_cooldown_until_ms = 0U;
    node->cluster.role_since_ms = 7U;
    node->cluster.next_join_retry_ms = 9U;
    node->cluster.pending_head_node_id = 0U;
    node->cluster.pending_cluster_id = 0U;
    node->cluster.pending_term = 0U;
    node->cluster.pending_head_score = 0U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_ELECTION; /* stale */
    node->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    node->cluster.shadow_transition_count = 0U;
    TEST_ASSERT(test_derive_phase(&node->cluster, 100U) ==
                UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    ucn_cluster_test_transition_asserts_set(false);
    result = ucn_cluster_test_consider_head_offer(&node->cluster, &candidate,
                                                  100U);
    ucn_cluster_test_transition_asserts_set(true);
    TEST_ASSERT(result == UCN_ERR_STATE);
    /* NOTHING of the join payload ran (fail closed). */
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(node->cluster.recovery_eligible == true);
    TEST_ASSERT(node->cluster.recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(node->cluster.pending_head_node_id == 0U);
    TEST_ASSERT(node->cluster.pending_cluster_id == 0U);
    TEST_ASSERT(node->cluster.pending_term == 0U);
    TEST_ASSERT(node->cluster.pending_head_score == 0U);
    TEST_ASSERT(node->cluster.role_since_ms == 7U);
    TEST_ASSERT(node->cluster.next_join_retry_ms == 9U);
    /* The shadow mirror is untouched (no transition, no sync mint). */
    TEST_ASSERT(node->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_RECOVERY_ELECTION);
    TEST_ASSERT(node->cluster.transition_reason == UCN_CLUSTER_REASON_INIT);
    TEST_ASSERT(node->cluster.shadow_transition_count == 0U);
    TEST_ASSERT(test_derive_phase(&node->cluster, 100U) ==
                UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    return 0;
}


/* CLV2-M11 (11-09): live score samples no longer mint a competing Term.
 * The legacy API is a no-write fail-closed stub until a separately gated,
 * durable planned-handover owner is connected. */
static int cluster_test_backup_challenge(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *backup;
    ucn_cluster_t before;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    backup = &network.nodes[1];
    backup->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
    backup->cluster.cluster_id = 1U;
    backup->cluster.term = 1U;
    backup->cluster.head_node_id = network.nodes[0].node_id;
    backup->cluster.backup_primary_node_id = network.nodes[0].node_id;
    backup->cluster.backup_ready = true;
    backup->cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;
    before = backup->cluster;
    TEST_ASSERT(ucn_cluster_test_backup_challenge(&backup->cluster, 60U) ==
                UCN_ERR_UNSUPPORTED);
    TEST_ASSERT(memcmp(&backup->cluster, &before, sizeof(before)) == 0);
    return 0;
}

/* CLV2-01-04a.1 (Item 1): the DIRECT edge list - every pair a SINGLE
 * production transition site can perform as one cluster_transition()
 * call.  The production CLUSTER_TRANSITION_DIRECT_ALLOWED must accept
 * EXACTLY this set (the 18x18 sweep below cross-checks both directions);
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
    { UCN_CLUSTER_PHASE_ELECTION, UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT },
    /* JOIN_PENDING */
    { UCN_CLUSTER_PHASE_JOIN_PENDING, UCN_CLUSTER_PHASE_MEMBER_ACTIVE },
    { UCN_CLUSTER_PHASE_JOIN_PENDING, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_JOIN_PENDING, UCN_CLUSTER_PHASE_BACKUP_SYNCING },
    { UCN_CLUSTER_PHASE_JOIN_PENDING, UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT },
    /* MEMBER_ACTIVE */
    { UCN_CLUSTER_PHASE_MEMBER_ACTIVE, UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE },
    { UCN_CLUSTER_PHASE_MEMBER_ACTIVE, UCN_CLUSTER_PHASE_BACKUP_SYNCING },
    { UCN_CLUSTER_PHASE_MEMBER_ACTIVE, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_MEMBER_ACTIVE, UCN_CLUSTER_PHASE_JOIN_PENDING },
    { UCN_CLUSTER_PHASE_MEMBER_ACTIVE, UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT },
    /* MEMBER_TAKEOVER_GRACE */
    { UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE, UCN_CLUSTER_PHASE_MEMBER_ACTIVE },
    { UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE, UCN_CLUSTER_PHASE_RECOVERY_OBSERVE },
    { UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE, UCN_CLUSTER_PHASE_JOIN_PENDING },
    { UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE, UCN_CLUSTER_PHASE_BACKUP_SYNCING },
    { UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE, UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT },
    /* HEAD_NO_BACKUP (NO_BACKUP->SYNCING / ->STABLE have no single site
     * and are never observed - dropped entirely) */
    { UCN_CLUSTER_PHASE_HEAD_NO_BACKUP, UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING },
    { UCN_CLUSTER_PHASE_HEAD_NO_BACKUP, UCN_CLUSTER_PHASE_STEPPING_DOWN },
    { UCN_CLUSTER_PHASE_HEAD_NO_BACKUP, UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT },
    /* HEAD_BACKUP_ASSIGNING */
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING, UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING, UCN_CLUSTER_PHASE_HEAD_STABLE },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING, UCN_CLUSTER_PHASE_HEAD_NO_BACKUP },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING, UCN_CLUSTER_PHASE_STEPPING_DOWN },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING, UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT },
    /* HEAD_BACKUP_SYNCING */
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING, UCN_CLUSTER_PHASE_HEAD_STABLE },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING, UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING, UCN_CLUSTER_PHASE_HEAD_NO_BACKUP },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING, UCN_CLUSTER_PHASE_STEPPING_DOWN },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING, UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT },
    /* HEAD_STABLE (->ASSIGNING is now DIRECT: backup_resync with an
     * armed sweep, CLV2-01-04d.7 ITEM 5) */
    { UCN_CLUSTER_PHASE_HEAD_STABLE, UCN_CLUSTER_PHASE_HEAD_NO_BACKUP },
    { UCN_CLUSTER_PHASE_HEAD_STABLE, UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING },
    { UCN_CLUSTER_PHASE_HEAD_STABLE, UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING },
    { UCN_CLUSTER_PHASE_HEAD_STABLE, UCN_CLUSTER_PHASE_STEPPING_DOWN },
    { UCN_CLUSTER_PHASE_HEAD_STABLE, UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT },
    /* BACKUP_SYNCING */
    { UCN_CLUSTER_PHASE_BACKUP_SYNCING, UCN_CLUSTER_PHASE_BACKUP_READY },
    { UCN_CLUSTER_PHASE_BACKUP_SYNCING, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_BACKUP_SYNCING, UCN_CLUSTER_PHASE_RECOVERY_OBSERVE },
    { UCN_CLUSTER_PHASE_BACKUP_SYNCING, UCN_CLUSTER_PHASE_ELECTION },
    { UCN_CLUSTER_PHASE_BACKUP_SYNCING, UCN_CLUSTER_PHASE_JOIN_PENDING },
    { UCN_CLUSTER_PHASE_BACKUP_SYNCING, UCN_CLUSTER_PHASE_MEMBER_ACTIVE },
    { UCN_CLUSTER_PHASE_BACKUP_SYNCING, UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT },
    /* BACKUP_READY (->HEAD_NO_BACKUP is an observed compound only) */
    { UCN_CLUSTER_PHASE_BACKUP_READY, UCN_CLUSTER_PHASE_BACKUP_TAKEOVER },
    { UCN_CLUSTER_PHASE_BACKUP_READY, UCN_CLUSTER_PHASE_BACKUP_SYNCING },
    { UCN_CLUSTER_PHASE_BACKUP_READY, UCN_CLUSTER_PHASE_ELECTION },
    { UCN_CLUSTER_PHASE_BACKUP_READY, UCN_CLUSTER_PHASE_JOIN_PENDING },
    { UCN_CLUSTER_PHASE_BACKUP_READY, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_BACKUP_READY, UCN_CLUSTER_PHASE_MEMBER_ACTIVE },
    { UCN_CLUSTER_PHASE_BACKUP_READY, UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT },
    /* BACKUP_TAKEOVER (stays legal under takeover_active && syncing) */
    { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, UCN_CLUSTER_PHASE_HEAD_NO_BACKUP },
    { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, UCN_CLUSTER_PHASE_JOIN_PENDING },
    { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, UCN_CLUSTER_PHASE_MEMBER_ACTIVE },
    { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, UCN_CLUSTER_PHASE_ELECTION },
    { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT },
    /* STEPPING_DOWN (deadline only; ->MEMBER_ACTIVE / ->DETACHED_OBSERVE
     * are observed compounds only) */
    { UCN_CLUSTER_PHASE_STEPPING_DOWN, UCN_CLUSTER_PHASE_JOIN_PENDING },
    { UCN_CLUSTER_PHASE_STEPPING_DOWN, UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT },
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
    { UCN_CLUSTER_PHASE_RECOVERY_HEAD, UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT },
    /* TERM_CONFLICT_WAIT: only higher authority can reopen a join. */
    { UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT, UCN_CLUSTER_PHASE_JOIN_PENDING },
};

/* CLV2-01-04b NIT-1: the tick-granularity COMPOUND pairs are NOT
 * duplicated here - the T-A gate checks the SINGLE production
 * CLUSTER_TRANSITION_OBSERVED_ALLOWED table via
 * ucn_cluster_test_observed_pair_allowed().  cluster_transition() must
 * reject them (wiring realizes them via their DIRECT constituents in
 * sequence); the 18x18 sweep below keeps the DIRECT test table pinned to
 * the production DIRECT table in both directions. */

/* CLV2-01-04a review A (F2): pairs the Current FSM can NEVER perform
 * (see the exclusion comment on CLUSTER_TRANSITION_ALLOWED for the per-pair
 * code evidence).  The matrix/spec must never admit them, and this list is
 * pinned below so a later edit that re-adds one fails the matrix test. */
static const struct cluster_transition_spec_pair CLUSTER_TRANSITION_EXCLUDED[] = {
    /* role CANDIDATE is written by start_election (DETACHED +
     * !recovery_eligible); no HEAD->ELECTION. */
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
    case UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT:
        cluster->role = UCN_CLUSTER_ROLE_TERM_CONFLICT;
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
 * (BACKUP_READY->HEAD_NO_BACKUP, STEPPING_DOWN->MEMBER_ACTIVE/
 * DETACHED_OBSERVE) are gate-only: they span a whole tick and are never
 * callable through cluster_transition().  HEAD_STABLE->HEAD_BACKUP_
 * ASSIGNING is now a REAL DIRECT edge (backup_resync with an armed sweep,
 * CLV2-01-04d.7 ITEM 5). */
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
                    /* CLV2-01-04d.1: apply_legacy already armed
                     * assign_pending (the phase-defining invariant), and the
                     * site write (assign_backup() L2677 + cycle L4045) stays
                     * idempotent - keep it here to mirror the real site. */
                    c->backup_node_id = 2U;
                    c->backup_assign_pending = true;
                    c->backup_ready = false;
                    break;
                case UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING:
                    /* site: sweep-done (send_backup_assignment_step() L4089,
                     * 01-04d.2 transition then pending=false). */
                    c->backup_node_id = 2U;
                    c->backup_assign_pending = false;
                    c->backup_ready = false;
                    break;
                case UCN_CLUSTER_PHASE_HEAD_STABLE:
                    /* site: handle_backup_ready() L2833 (ready=true). */
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

/* CLV2-01-04d.0: preflight is pure validation with ZERO writes - it must
 * never advance the shadow / reason / count or touch legacy fields, on
 * either the accept or the reject path (the d-group irreversible-site
 * pattern relies on this: preflight before the Current-order side
 * effects, commit afterwards). */
static int cluster_test_transition_preflight(void)
{
    cluster_test_network_t network;
    ucn_cluster_t pristine;
    ucn_cluster_t before;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 0U;
    network.nodes[0].cluster.backup_node_id = 2U;
    pristine = network.nodes[0].cluster;

    ucn_cluster_test_transition_asserts_set(false);
    {
        ucn_cluster_t *c = &network.nodes[0].cluster;

        /* (a) legal DIRECT pair, legacy + shadow aligned: UCN_OK and NO
         * write of any kind. */
        cluster_test_transition_reset(c, &pristine,
                                      UCN_CLUSTER_PHASE_JOIN_PENDING);
        cluster_test_seed_legacy(c, UCN_CLUSTER_PHASE_JOIN_PENDING,
                                 network.now_ms);
        before = *c;
        TEST_ASSERT(ucn_cluster_test_transition_preflight(
                        c, UCN_CLUSTER_PHASE_JOIN_PENDING,
                        UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                        network.now_ms) == UCN_OK);
        TEST_ASSERT(memcmp(c, &before, sizeof(*c)) == 0);

        /* (b1) illegal pair (JOIN_PENDING -> HEAD_STABLE is not DIRECT):
         * UCN_ERR_STATE and NO write. */
        cluster_test_transition_reset(c, &pristine,
                                      UCN_CLUSTER_PHASE_JOIN_PENDING);
        cluster_test_seed_legacy(c, UCN_CLUSTER_PHASE_JOIN_PENDING,
                                 network.now_ms);
        before = *c;
        TEST_ASSERT(ucn_cluster_test_transition_preflight(
                        c, UCN_CLUSTER_PHASE_JOIN_PENDING,
                        UCN_CLUSTER_PHASE_HEAD_STABLE,
                        network.now_ms) == UCN_ERR_STATE);
        TEST_ASSERT(memcmp(c, &before, sizeof(*c)) == 0);

        /* (b2) shadow desync (shadow != claimed old_phase): UCN_ERR_STATE
         * and NO write - the d-group pattern relies on this to abort
         * BEFORE the irreversible site side effects run. */
        cluster_test_transition_reset(c, &pristine,
                                      UCN_CLUSTER_PHASE_JOIN_PENDING);
        cluster_test_seed_legacy(c, UCN_CLUSTER_PHASE_JOIN_PENDING,
                                 network.now_ms);
        c->shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE; /* desync */
        before = *c;
        TEST_ASSERT(ucn_cluster_test_transition_preflight(
                        c, UCN_CLUSTER_PHASE_JOIN_PENDING,
                        UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                        network.now_ms) == UCN_ERR_STATE);
        TEST_ASSERT(memcmp(c, &before, sizeof(*c)) == 0);
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
    /* 01-04d.1: apply_legacy already armed pending on the ASSIGNING
     * entry; the site's start_backup_assignment_cycle() L4045 write stays
     * (idempotent same value). */
    c->backup_assign_pending = true;
    TEST_ASSERT(c->backup_generation == 5U);
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING,
                    UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING,
                    UCN_CLUSTER_REASON_BACKUP_SYNC_STARTED, now_ms) == UCN_OK);
    /* 01-04d.2: sweep-done transition ASSIGNING -> SYNCING ran in the
     * call above; the site's pending=false write stays (sweep L4089). */
    c->backup_assign_pending = false;
    TEST_ASSERT(c->backup_generation == 5U);
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING,
                    UCN_CLUSTER_PHASE_HEAD_STABLE,
                    UCN_CLUSTER_REASON_SNAPSHOT_READY, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_HEAD_STABLE);
    c->backup_ready = true; /* site: handle_backup_ready L2833 */
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

    /* 11) Historical generic FSM relation: the transition engine itself
     *     preserves members[] and backup_generation for BACKUP_SYNCING ->
     *     ELECTION.  M11 removes the old v3 score-triggered production site;
     *     this remains a transition-table contract, not a reachable score
     *     path. */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->backup_syncing = true;
    c->backup_ready = false;
    c->backup_takeover_active = false;
    c->backup_primary_node_id = 1U;
    c->backup_generation = 5U;
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = 4U;
    c->primary_members.slots[0].last_nonce = 9U;
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_BACKUP_SYNCING,
                    UCN_CLUSTER_PHASE_ELECTION,
                    UCN_CLUSTER_REASON_ELECTION_STARTED, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_ELECTION);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_CANDIDATE);
    /* members[] and backup_generation survive (no mirror wipe). */
    TEST_ASSERT(c->primary_members.slots[0].occupied == true);
    TEST_ASSERT(c->primary_members.slots[0].node_id == 4U);
    TEST_ASSERT(c->backup_generation == 5U);
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_ELECTION,
                    UCN_CLUSTER_PHASE_JOIN_PENDING,
                    UCN_CLUSTER_REASON_JOIN_INITIATED, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(c->primary_members.slots[0].occupied == true); /* begin_join keeps it */
    TEST_ASSERT(c->backup_generation == 5U);
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_JOIN_PENDING);

    ucn_cluster_test_transition_asserts_set(true);
    return 0;
}

/* CLV2-01-04d.1: assign_backup() commits the NO_BACKUP -> ASSIGNING
 * transition BEFORE the node_id write.  A Head with no Backup selects the
 * best head-capable candidate: the shadow moves NO_BACKUP -> ASSIGNING with
 * reason BACKUP_ASSIGNED, apply_legacy arms assign_pending, and the site
 * writes node_id - so the derive is ASSIGNING at the end of the step (never
 * a bogus SYNCING) and no extra tick-end pair is minted. */
static int cluster_test_assign_backup_transition(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    ucn_cluster_t *c;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];
    c = &head->cluster;
    /* Head with no Backup, two members, both head-capable candidates. */
    c->role = UCN_CLUSTER_ROLE_HEAD;
    c->cluster_id = 1U;
    c->term = 1U;
    c->head_node_id = head->node_id;
    c->backup_node_id = 0U;
    c->backup_generation = 1U;
    c->backup_ready = false;
    c->backup_assign_pending = false;
    c->backup_syncing = false;
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = network.nodes[1].node_id;
    c->primary_members.slots[0].lease_expires_at_ms = 100U;
    c->primary_members.slots[1].occupied = true;
    c->primary_members.slots[1].node_id = network.nodes[2].node_id;
    c->primary_members.slots[1].lease_expires_at_ms = 100U;
    c->candidates[0].occupied = true;
    c->candidates[0].head_node_id = network.nodes[1].node_id;
    c->candidates[0].head_score = 3000U;
    c->candidates[1].occupied = true;
    c->candidates[1].head_node_id = network.nodes[2].node_id;
    c->candidates[1].head_score = 2000U;
    /* The framework shadow must mirror the legacy state before the step
     * (a real NO_BACKUP Head starts its step like this). */
    c->shadow_phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    network.now_ms = 0U;

    TEST_ASSERT(ucn_cluster_step(c) == UCN_OK);
    /* d.1: shadow NO_BACKUP -> ASSIGNING with the explicit reason, pending
     * armed by apply_legacy, node_id set by the site, derive ASSIGNING. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_BACKUP_ASSIGNED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->backup_assign_pending == true);
    TEST_ASSERT(c->backup_node_id == network.nodes[1].node_id);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
    return 0;
}

/* CLV2-01-04d.2: the assignment sweep completion commits the ASSIGNING ->
 * SYNCING transition BEFORE the pending=false write.  After the last ASSIGN
 * frame is sent the shadow moves ASSIGNING -> SYNCING with reason
 * BACKUP_SYNC_STARTED, pending clears, and the derive is SYNCING. */
static int cluster_test_backup_assignment_sweep_transition(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    ucn_cluster_t *c;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];
    c = &head->cluster;
    /* Head mid-sweep: Backup selected, one ASSIGN frame still queued. */
    c->role = UCN_CLUSTER_ROLE_HEAD;
    c->cluster_id = 1U;
    c->term = 1U;
    c->head_node_id = head->node_id;
    c->backup_node_id = network.nodes[1].node_id;
    c->backup_generation = 1U;
    c->backup_ready = false;
    c->backup_assign_pending = true;
    c->backup_assign_cursor = 0U;
    c->backup_assign_remaining = 1U;
    c->backup_syncing = false;
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = network.nodes[1].node_id;
    c->primary_members.slots[0].lease_expires_at_ms = 100U;
    c->candidates[0].occupied = true;
    c->candidates[0].head_node_id = network.nodes[1].node_id;
    c->candidates[0].head_score = 3000U;
    /* Consistent framework shadow: the sweep is armed, so ASSIGNING. */
    c->shadow_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING;
    c->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    c->shadow_transition_count = 0U;
    network.now_ms = 0U;

    TEST_ASSERT(ucn_cluster_step(c) == UCN_OK);
    /* d.2: sweep done -> shadow ASSIGNING -> SYNCING with the explicit
     * reason, pending cleared by the site, derive SYNCING. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_BACKUP_SYNC_STARTED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->backup_assign_pending == false);
    TEST_ASSERT(c->backup_assign_remaining == 0U);
    TEST_ASSERT(c->backup_node_id == network.nodes[1].node_id);
    TEST_ASSERT(test_derive_phase(c, network.now_ms) ==
                UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
    return 0;
}

/* CLV2-01-04d.4: remove_member() / expire_members() backup-eviction
 * preflight wiring (the FIRST irreversible-site migration).
 *   (a) a STABLE Head evicts its Backup (LEAVE) -> shadow==NO_BACKUP,
 *       reason==BACKUP_LOST, member slot freed, node_id=0 / ready=false;
 *   (b) same from SYNCING and ASSIGNING old phases;
 *   (c) a non-backup member eviction keeps the legacy path: NO transition
 *       (shadow unchanged, count 0);
 *   (d) fail-closed: shadow desync + backup eviction -> preflight rejects
 *       with UCN_ERR_STATE and NOTHING is touched (memcmp zero-write
 *       proof - the zero-side-effect-on-validation-failure invariant).
 * The hooks drive the static sites directly (no network tick), so no
 * observed phase pair is recorded and the T-A gate keeps the suite count. */
static int cluster_test_remove_member_backup_loss(void)
{
    cluster_test_network_t network;
    ucn_cluster_t *c;
    ucn_cluster_t pristine;
    const uint32_t now_ms = 100U;
    static const ucn_cluster_phase_t phases[3] = {
        UCN_CLUSTER_PHASE_HEAD_STABLE,
        UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING,
        UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING
    };
    size_t index;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    c = &network.nodes[0].cluster;
    network.now_ms = now_ms;
    pristine = *c;

    ucn_cluster_test_transition_asserts_set(false);

    /* (a)+(b) remove_member: backup eviction from every Head sub-phase. */
    for (index = 0U; index < 3U; ++index) {
        ucn_cluster_phase_t phase = phases[index];

        cluster_test_transition_reset(c, &pristine, phase);
        cluster_test_seed_legacy(c, phase, now_ms);
        c->primary_members.slots[0].occupied = true;
        c->primary_members.slots[0].node_id = 2U; /* the Backup */
        c->primary_members.slots[0].last_nonce = 7U;
        c->primary_members.slots[1].occupied = true;
        c->primary_members.slots[1].node_id = 3U; /* ordinary member */
        c->primary_members.slots[1].last_nonce = 9U;
        TEST_ASSERT(test_derive_phase(c, now_ms) == phase);

        ucn_cluster_test_remove_member(c, 2U, now_ms);

        TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
        TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_BACKUP_LOST);
        TEST_ASSERT(c->shadow_transition_count == 1U);
        TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_HEAD);
        TEST_ASSERT(c->primary_members.slots[0].occupied == false); /* slot freed */
        TEST_ASSERT(c->primary_members.slots[1].occupied == true);  /* other member lives */
        TEST_ASSERT(c->primary_members.slots[1].node_id == 3U);
        TEST_ASSERT(c->backup_node_id == 0U);
        TEST_ASSERT(c->backup_ready == false);
        TEST_ASSERT(test_derive_phase(c, now_ms) ==
                    UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    }

    /* (a)+(b) expire_members: the Backup's lease expires -> same result. */
    for (index = 0U; index < 3U; ++index) {
        ucn_cluster_phase_t phase = phases[index];

        cluster_test_transition_reset(c, &pristine, phase);
        cluster_test_seed_legacy(c, phase, now_ms);
        c->primary_members.slots[0].occupied = true;
        c->primary_members.slots[0].node_id = 2U; /* the Backup, expired */
        c->primary_members.slots[0].lease_expires_at_ms = now_ms - 1U;
        c->primary_members.slots[1].occupied = true;
        c->primary_members.slots[1].node_id = 3U; /* live member */
        c->primary_members.slots[1].lease_expires_at_ms = now_ms + 1000U;
        TEST_ASSERT(test_derive_phase(c, now_ms) == phase);

        ucn_cluster_test_expire_members(c, now_ms);

        TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
        TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_BACKUP_LOST);
        TEST_ASSERT(c->shadow_transition_count == 1U);
        TEST_ASSERT(c->primary_members.slots[0].occupied == false); /* backup evicted */
        TEST_ASSERT(c->primary_members.slots[1].occupied == true);  /* live member stays */
        TEST_ASSERT(c->backup_node_id == 0U);
        TEST_ASSERT(c->backup_ready == false);
        TEST_ASSERT(c->stats.member_leases_expired == 1U);
        TEST_ASSERT(test_derive_phase(c, now_ms) ==
                    UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    }

    /* (c) remove_member: non-backup eviction -> NO BACKUP_LOST transition;
     * but the legacy backup_resync() still runs (Current behaviour), and with
     * d.5 merged that resync is itself a STABLE->SYNCING transition. */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_HEAD_STABLE);
    cluster_test_seed_legacy(c, UCN_CLUSTER_PHASE_HEAD_STABLE, now_ms);
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = 2U; /* the Backup */
    c->primary_members.slots[1].occupied = true;
    c->primary_members.slots[1].node_id = 3U;
    ucn_cluster_test_remove_member(c, 3U, now_ms);
    /* the backup-loss path was NOT taken (reason must not be BACKUP_LOST);
     * the resync transition (d.5) is the correct Current semantics. */
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RESYNC_STARTED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->primary_members.slots[1].occupied == false); /* evicted */
    TEST_ASSERT(c->primary_members.slots[0].occupied == true);  /* backup survives */
    TEST_ASSERT(c->backup_node_id == 2U);
    /* legacy backup_resync() ran: ready cleared, snapshot reset. */
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->backup_sync_cursor == 0U);

    /* (c-expire) expire_members: non-backup expiry -> NO BACKUP_LOST;
     * the legacy resync (d.5) still transitions STABLE->SYNCING. */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_HEAD_STABLE);
    cluster_test_seed_legacy(c, UCN_CLUSTER_PHASE_HEAD_STABLE, now_ms);
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = 2U; /* the Backup, live */
    c->primary_members.slots[0].lease_expires_at_ms = now_ms + 1000U;
    c->primary_members.slots[1].occupied = true;
    c->primary_members.slots[1].node_id = 3U; /* expired */
    c->primary_members.slots[1].lease_expires_at_ms = now_ms - 1U;
    ucn_cluster_test_expire_members(c, now_ms);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_RESYNC_STARTED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->primary_members.slots[1].occupied == false);
    TEST_ASSERT(c->primary_members.slots[0].occupied == true);
    TEST_ASSERT(c->backup_node_id == 2U);
    TEST_ASSERT(c->stats.member_leases_expired == 1U);

    /* (d) remove_member: shadow desync + backup eviction -> preflight
     * rejects and NOTHING is touched (memcmp zero-write proof). */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_HEAD_STABLE);
    cluster_test_seed_legacy(c, UCN_CLUSTER_PHASE_HEAD_STABLE, now_ms);
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = 2U;
    c->primary_members.slots[1].occupied = true;
    c->primary_members.slots[1].node_id = 3U;
    c->shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE; /* desync */
    {
        ucn_cluster_t before = *c;

        ucn_cluster_test_remove_member(c, 2U, now_ms);
        TEST_ASSERT(memcmp(c, &before, sizeof(*c)) == 0);
    }

    /* (d-expire) expire_members: same invariant - the expired Backup's
     * slot is NOT freed and no other expired member is evicted either
     * (the whole pass aborts before any write). */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_HEAD_STABLE);
    cluster_test_seed_legacy(c, UCN_CLUSTER_PHASE_HEAD_STABLE, now_ms);
    c->primary_members.slots[0].occupied = true;
    c->primary_members.slots[0].node_id = 2U;
    c->primary_members.slots[0].lease_expires_at_ms = now_ms - 1U;
    c->primary_members.slots[1].occupied = true;
    c->primary_members.slots[1].node_id = 3U;
    c->primary_members.slots[1].lease_expires_at_ms = now_ms - 1U; /* also expired */
    c->shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE; /* desync */
    {
        ucn_cluster_t before = *c;

        ucn_cluster_test_expire_members(c, now_ms);
        TEST_ASSERT(memcmp(c, &before, sizeof(*c)) == 0);
    }

    ucn_cluster_test_transition_asserts_set(true);
    return 0;
}

/* CLV2-01-04d.7 (HEAD-Ladder Closure): every head-ladder phase-relevant
 * write site now either keeps the phase unchanged or flows through an
 * explicit cluster_transition()/preflight - no end-of-step shadow_sync()
 * minting is relied on.  These cases pin the four new wiring points:
 * (a) start_backup_assignment_cycle SYNCING->ASSIGNING + self re-arm;
 * (b) handle_backup_reject full STABLE->NO_BACKUP->ASSIGNING chain;
 * (c) backup_resync with an armed sweep -> direct STABLE->ASSIGNING;
 * (d) sweep-done last-frame preflight failure leaves the sweep untouched,
 *     then the restored state completes with an explicit SYNCING. */
static int cluster_test_head_ladder_closure(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    ucn_cluster_t pristine;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    uint32_t now_ms = 0U;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = now_ms;
    head = &network.nodes[0];
    pristine = network.nodes[0].cluster;

    /* (a) start_backup_assignment_cycle: a SYNCING head arming the sweep
     * transitions SYNCING -> ASSIGNING (BACKUP_ASSIGNED) BEFORE the
     * pending write; a second call (already ASSIGNING) is a self re-arm
     * with NO new transition. */
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = network.nodes[1].node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.backup_ready = false;
    head->cluster.backup_assign_pending = false;
    head->cluster.backup_assign_remaining = 0U;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
    head->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    head->cluster.shadow_transition_count = 0U;
    head->cluster.primary_members.slots[0].occupied = true;
    head->cluster.primary_members.slots[0].node_id = network.nodes[1].node_id;
    ucn_cluster_test_start_backup_assignment_cycle(&head->cluster, now_ms);
    TEST_ASSERT(head->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
    TEST_ASSERT(head->cluster.transition_reason ==
                UCN_CLUSTER_REASON_BACKUP_ASSIGNED);
    TEST_ASSERT(head->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(head->cluster.backup_assign_pending == true);
    TEST_ASSERT(head->cluster.backup_assign_remaining == 1U);
    /* self re-arm: already ASSIGNING -> no transition, no count bump. */
    ucn_cluster_test_start_backup_assignment_cycle(&head->cluster, now_ms);
    TEST_ASSERT(head->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
    TEST_ASSERT(head->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(head->cluster.transition_reason ==
                UCN_CLUSTER_REASON_BACKUP_ASSIGNED);

    /* (b) handle_backup_reject: STABLE -> NO_BACKUP (BACKUP_LOST) via the
     * entry point, then the reslection commits NO_BACKUP -> ASSIGNING
     * (BACKUP_ASSIGNED) with an aligned shadow guard - TWO explicit
     * transitions, no stale-shadow fallback (count == 2). */
    cluster_test_transition_reset(&head->cluster, &pristine,
                                  UCN_CLUSTER_PHASE_HEAD_STABLE);
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = network.nodes[1].node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.backup_ready = true;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_STABLE;
    head->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    head->cluster.shadow_transition_count = 0U;
    head->cluster.primary_members.slots[0].occupied = true;
    head->cluster.primary_members.slots[0].node_id = network.nodes[1].node_id;
    head->cluster.primary_members.slots[1].occupied = true;
    head->cluster.primary_members.slots[1].node_id = network.nodes[2].node_id;
    head->cluster.candidates[0].occupied = true;
    head->cluster.candidates[0].head_node_id = network.nodes[1].node_id;
    head->cluster.candidates[0].head_score = 3000U;
    head->cluster.candidates[1].occupied = true;
    head->cluster.candidates[1].head_node_id = network.nodes[2].node_id;
    head->cluster.candidates[1].head_score = 2000U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_REJECT;
    message.role = UCN_CLUSTER_ROLE_BACKUP;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.reject_reason = UCN_CLUSTER_BACKUP_REJECT_COVERAGE;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster,
                                    network.nodes[1].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
    TEST_ASSERT(head->cluster.shadow_transition_count == 2U);
    TEST_ASSERT(head->cluster.transition_reason ==
                UCN_CLUSTER_REASON_BACKUP_ASSIGNED);
    TEST_ASSERT(head->cluster.backup_node_id == network.nodes[2].node_id);
    TEST_ASSERT(head->cluster.backup_rejected_node_id ==
                network.nodes[1].node_id);
    TEST_ASSERT(head->cluster.backup_candidate_cooldown_until_ms != 0U);

    /* (c) backup_resync with an armed sweep: a STABLE head (ready == true
     * keeps derive STABLE via ready precedence) with pending armed gets a
     * RESYNC_REQ - the destination is dispatched to ASSIGNING and the
     * transition is REAL and DIRECT (RESYNC_STARTED). */
    cluster_test_transition_reset(&head->cluster, &pristine,
                                  UCN_CLUSTER_PHASE_HEAD_STABLE);
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = network.nodes[1].node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.backup_ready = true;
    head->cluster.backup_assign_pending = true; /* armed sweep window */
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_STABLE;
    head->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    head->cluster.shadow_transition_count = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ;
    message.role = UCN_CLUSTER_ROLE_BACKUP;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster,
                                    network.nodes[1].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
    TEST_ASSERT(head->cluster.transition_reason ==
                UCN_CLUSTER_REASON_RESYNC_STARTED);
    TEST_ASSERT(head->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(head->cluster.backup_ready == false);
    TEST_ASSERT(head->cluster.backup_assign_pending == true);

    /* ================ CLV2-01-04d.7.1 mirror-symmetric shadow-desync
     * cases: legacy-right / shadow-wrong.  The shadow-guard rule says the
     * LEGACY decides which transition to attempt; a corrupted Shadow must
     * fail-closed inside cluster_transition()/preflight - never be skipped
     * via a shadow-based guard.  The knob is off for all of (d)-(h) so the
     * rejections run the release path. ================ */

    /* (d) last-frame shadow-desync: legacy derives ASSIGNING, shadow is
     * STABLE -> the preflight fails BEFORE the last ASSIGN is sent and NO
     * sweep state moves; once the shadow is restored the next call sends
     * and commits the explicit ASSIGNING -> SYNCING. */
    cluster_test_transition_reset(&head->cluster, &pristine,
                                  UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = network.nodes[1].node_id;
    head->cluster.backup_generation = 1U; /* valid BACKUP_ASSIGN encode */
    head->cluster.backup_ready = false;
    head->cluster.backup_assign_pending = true;
    head->cluster.backup_assign_remaining = 1U;
    head->cluster.backup_assign_cursor = 0U;
    head->cluster.next_backup_assign_ms = 0U;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_STABLE; /* desync */
    head->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    head->cluster.shadow_transition_count = 0U;
    head->cluster.primary_members.slots[0].occupied = true;
    head->cluster.primary_members.slots[0].node_id = network.nodes[1].node_id;
    network.queue_count = 0U;
    ucn_cluster_test_transition_asserts_set(false);
    ucn_cluster_test_send_backup_assignment_step(&head->cluster, now_ms);
    TEST_ASSERT(network.queue_count == 0U); /* nothing sent */
    TEST_ASSERT(head->cluster.backup_assign_remaining == 1U);
    TEST_ASSERT(head->cluster.backup_assign_pending == true);
    TEST_ASSERT(head->cluster.backup_assign_cursor == 0U);
    TEST_ASSERT(head->cluster.shadow_phase == UCN_CLUSTER_PHASE_HEAD_STABLE);
    TEST_ASSERT(head->cluster.shadow_transition_count == 0U);
    /* restore the shadow: derive ASSIGNING + shadow ASSIGNING -> the next
     * call sends and commits the explicit ASSIGNING -> SYNCING. */
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING;
    ucn_cluster_test_send_backup_assignment_step(&head->cluster, now_ms);
    TEST_ASSERT(network.queue_count == 1U); /* the last ASSIGN was sent */
    TEST_ASSERT(head->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
    TEST_ASSERT(head->cluster.transition_reason ==
                UCN_CLUSTER_REASON_BACKUP_SYNC_STARTED);
    TEST_ASSERT(head->cluster.shadow_transition_count == 1U);
    TEST_ASSERT(head->cluster.backup_assign_pending == false);
    TEST_ASSERT(head->cluster.backup_assign_remaining == 0U);

    /* (e) cycle shadow-desync: legacy derives SYNCING, shadow is STABLE ->
     * the SYNCING -> ASSIGNING transition is attempted UNCONDITIONALLY and
     * fails fail-closed: the sweep is NOT armed and no sweep state moves. */
    cluster_test_transition_reset(&head->cluster, &pristine,
                                  UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = network.nodes[1].node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.backup_ready = false;
    head->cluster.backup_assign_pending = false;
    head->cluster.backup_assign_remaining = 9U; /* prove untouched */
    head->cluster.backup_assign_cursor = 7U;    /* prove untouched */
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_STABLE; /* desync */
    head->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    head->cluster.shadow_transition_count = 0U;
    head->cluster.primary_members.slots[0].occupied = true;
    head->cluster.primary_members.slots[0].node_id = network.nodes[1].node_id;
    ucn_cluster_test_start_backup_assignment_cycle(&head->cluster, now_ms);
    TEST_ASSERT(head->cluster.backup_assign_pending == false); /* NOT armed */
    TEST_ASSERT(head->cluster.backup_assign_remaining == 9U);
    TEST_ASSERT(head->cluster.backup_assign_cursor == 7U);
    TEST_ASSERT(head->cluster.shadow_phase == UCN_CLUSTER_PHASE_HEAD_STABLE);
    TEST_ASSERT(head->cluster.shadow_transition_count == 0U);

    /* (f) queue shadow-desync: legacy derives SYNCING, shadow is
     * DETACHED_OBSERVE -> the targeted assignment is NOT armed. */
    cluster_test_transition_reset(&head->cluster, &pristine,
                                  UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = network.nodes[1].node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.backup_ready = false;
    head->cluster.backup_assign_pending = false;
    head->cluster.backup_assign_remaining = 5U; /* prove untouched */
    head->cluster.backup_assign_cursor = 3U;    /* prove untouched */
    head->cluster.shadow_phase =
        UCN_CLUSTER_PHASE_DETACHED_OBSERVE; /* desync */
    head->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    head->cluster.shadow_transition_count = 0U;
    head->cluster.primary_members.slots[0].occupied = true;
    head->cluster.primary_members.slots[0].node_id = network.nodes[1].node_id;
    ucn_cluster_test_queue_backup_assignment_for_member(
        &head->cluster, network.nodes[1].node_id, now_ms);
    TEST_ASSERT(head->cluster.backup_assign_pending == false); /* NOT armed */
    TEST_ASSERT(head->cluster.backup_assign_remaining == 5U);
    TEST_ASSERT(head->cluster.backup_assign_cursor == 3U);
    TEST_ASSERT(head->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(head->cluster.shadow_transition_count == 0U);

    /* (g) assign_backup shadow-desync: legacy derives NO_BACKUP (node_id
     * == 0), a candidate is available, shadow is STABLE -> the
     * NO_BACKUP -> ASSIGNING transition is attempted UNCONDITIONALLY and
     * fails fail-closed: the selection is NOT committed, the generation is
     * NOT incremented and no assignment is armed. */
    cluster_test_transition_reset(&head->cluster, &pristine,
                                  UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = 0U;
    head->cluster.backup_generation = 5U;
    head->cluster.backup_ready = false;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_STABLE; /* desync */
    head->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    head->cluster.shadow_transition_count = 0U;
    head->cluster.primary_members.slots[0].occupied = true;
    head->cluster.primary_members.slots[0].node_id = network.nodes[1].node_id;
    head->cluster.candidates[0].occupied = true;
    head->cluster.candidates[0].head_node_id = network.nodes[1].node_id;
    head->cluster.candidates[0].head_score = 3000U;
    ucn_cluster_test_assign_backup(&head->cluster, now_ms);
    TEST_ASSERT(head->cluster.backup_node_id == 0U); /* NOT committed */
    TEST_ASSERT(head->cluster.backup_generation == 5U); /* NOT incremented */
    TEST_ASSERT(head->cluster.backup_assign_pending == false);
    TEST_ASSERT(head->cluster.backup_assign_remaining == 0U);
    TEST_ASSERT(head->cluster.shadow_phase == UCN_CLUSTER_PHASE_HEAD_STABLE);
    TEST_ASSERT(head->cluster.shadow_transition_count == 0U);

    /* (h) loop-exhausted shadow-desync: legacy derives ASSIGNING, shadow
     * is STABLE, no occupied member left to sweep -> the loop-exhausted
     * ASSIGNING -> SYNCING transition is attempted UNCONDITIONALLY and
     * fails fail-closed: pending is NOT cleared and remaining is NOT
     * zeroed (the phase is not silently moved). */
    cluster_test_transition_reset(&head->cluster, &pristine,
                                  UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = network.nodes[1].node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.backup_ready = false;
    head->cluster.backup_assign_pending = true;
    head->cluster.backup_assign_remaining = 2U; /* > 1: no preflight */
    head->cluster.backup_assign_cursor = 0U;
    head->cluster.next_backup_assign_ms = 0U;
    head->cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_STABLE; /* desync */
    head->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    head->cluster.shadow_transition_count = 0U;
    /* no occupied members: the sweep loop exhausts */
    ucn_cluster_test_send_backup_assignment_step(&head->cluster, now_ms);
    TEST_ASSERT(head->cluster.backup_assign_pending == true); /* NOT cleared */
    TEST_ASSERT(head->cluster.backup_assign_remaining == 2U); /* NOT zeroed */
    TEST_ASSERT(head->cluster.shadow_phase == UCN_CLUSTER_PHASE_HEAD_STABLE);
    TEST_ASSERT(head->cluster.shadow_transition_count == 0U);

    ucn_cluster_test_transition_asserts_set(true);
    return 0;
}

/* CLV2-M03 (03-01): Epoch comparator boundary tests - pure infrastructure.
 * Relation semantics (human auditor, frozen):
 *   same cluster -> terms comparable; same term + same head -> SAME,
 *   same term + different head -> CONFLICT; different cluster -> FOREIGN
 *   (terms NEVER compared across clusters - the foreign domain is
 *   truncated FIRST, so Cluster A term 2 vs Cluster B term 100 is FOREIGN,
 *   never HIGHER).  This test ONLY pins the comparator mathematics; no
 * production decision path is touched by 03-01. */
static int cluster_test_epoch_comparator(void)
{
    ucn_cluster_epoch_t a;
    ucn_cluster_epoch_t b;

    /* (a) SAME: identical epoch. */
    a.cluster_id = 1U; a.term = 5U; a.head_node_id = 2U;
    b.cluster_id = 1U; b.term = 5U; b.head_node_id = 2U;
    TEST_ASSERT(ucn_cluster_epoch_compare(&a, &b) ==
                UCN_CLUSTER_EPOCH_RELATION_SAME);
    TEST_ASSERT(ucn_cluster_epoch_is_same_cluster(&a, &b) == true);
    TEST_ASSERT(ucn_cluster_epoch_is_foreign(&a, &b) == false);

    /* (b) LOWER / HIGHER: same cluster, different term. */
    a.cluster_id = 1U; a.term = 3U; a.head_node_id = 2U;
    b.cluster_id = 1U; b.term = 5U; b.head_node_id = 2U;
    TEST_ASSERT(ucn_cluster_epoch_compare(&a, &b) ==
                UCN_CLUSTER_EPOCH_RELATION_LOWER);
    TEST_ASSERT(ucn_cluster_epoch_compare(&b, &a) ==
                UCN_CLUSTER_EPOCH_RELATION_HIGHER);
    TEST_ASSERT(ucn_cluster_epoch_is_same_cluster(&a, &b) == true);

    /* (c) CONFLICT: same cluster, same term, different Head. */
    a.cluster_id = 1U; a.term = 5U; a.head_node_id = 2U;
    b.cluster_id = 1U; b.term = 5U; b.head_node_id = 3U;
    TEST_ASSERT(ucn_cluster_epoch_compare(&a, &b) ==
                UCN_CLUSTER_EPOCH_RELATION_CONFLICT);
    TEST_ASSERT(ucn_cluster_epoch_compare(&b, &a) ==
                UCN_CLUSTER_EPOCH_RELATION_CONFLICT); /* symmetric */

    /* (d) FOREIGN: different cluster_id - terms are NEVER compared.
     * The critical case: Cluster A term 2 vs Cluster B term 100 must be
     * FOREIGN (not HIGHER) - the foreign domain truncates first. */
    a.cluster_id = 1U; a.term = 2U; a.head_node_id = 1U;
    b.cluster_id = 2U; b.term = 100U; b.head_node_id = 9U;
    TEST_ASSERT(ucn_cluster_epoch_compare(&a, &b) ==
                UCN_CLUSTER_EPOCH_RELATION_FOREIGN);
    TEST_ASSERT(ucn_cluster_epoch_compare(&b, &a) ==
                UCN_CLUSTER_EPOCH_RELATION_FOREIGN); /* symmetric */
    TEST_ASSERT(ucn_cluster_epoch_is_foreign(&a, &b) == true);
    TEST_ASSERT(ucn_cluster_epoch_is_same_cluster(&a, &b) == false);
    /* Foreign with the same term number is STILL foreign (cluster is the
     * comparison domain, not the term). */
    a.cluster_id = 1U; a.term = 7U; a.head_node_id = 1U;
    b.cluster_id = 2U; b.term = 7U; b.head_node_id = 1U;
    TEST_ASSERT(ucn_cluster_epoch_compare(&a, &b) ==
                UCN_CLUSTER_EPOCH_RELATION_FOREIGN);

    /* (e) NULL safety. */
    TEST_ASSERT(ucn_cluster_epoch_compare(NULL, &b) ==
                UCN_CLUSTER_EPOCH_RELATION_UNKNOWN);
    TEST_ASSERT(ucn_cluster_epoch_compare(&a, NULL) ==
                UCN_CLUSTER_EPOCH_RELATION_UNKNOWN);
    TEST_ASSERT(ucn_cluster_epoch_is_same_cluster(NULL, &b) == false);
    TEST_ASSERT(ucn_cluster_epoch_is_foreign(&a, NULL) == false);

    /* (f) Term boundary: 0 is a valid term (recovery starts at term 1,
     * but the comparator must handle any uint32). */
    a.cluster_id = 1U; a.term = 0U; a.head_node_id = 1U;
    b.cluster_id = 1U; b.term = 1U; b.head_node_id = 1U;
    TEST_ASSERT(ucn_cluster_epoch_compare(&a, &b) ==
                UCN_CLUSTER_EPOCH_RELATION_LOWER);
    return 0;
}

/* CLV2-M03.1 (03-01 NIT-1 closure, human audit): deterministic property
 * sanity - the comparator is antisymmetric on the comparable domain
 * (LOWER/HIGHER swap, SAME stable) and symmetric on CONFLICT / FOREIGN.
 * 03-09 adds the full random-seed property suite; these cheap assertions
 * pin the algebraic skeleton now. */
static int cluster_test_epoch_property_sanity(void)
{
    ucn_cluster_epoch_t a;
    ucn_cluster_epoch_t b;

    /* LOWER/HIGHER are antisymmetric: compare(a,b)==LOWER implies
     * compare(b,a)==HIGHER. */
    a.cluster_id = 1U; a.term = 3U; a.head_node_id = 2U;
    b.cluster_id = 1U; b.term = 5U; b.head_node_id = 2U;
    TEST_ASSERT(ucn_cluster_epoch_compare(&a, &b) ==
                UCN_CLUSTER_EPOCH_RELATION_LOWER);
    TEST_ASSERT(ucn_cluster_epoch_compare(&b, &a) ==
                UCN_CLUSTER_EPOCH_RELATION_HIGHER);

    /* SAME is antisymmetric (both directions SAME). */
    a.cluster_id = 2U; a.term = 4U; a.head_node_id = 1U;
    b.cluster_id = 2U; b.term = 4U; b.head_node_id = 1U;
    TEST_ASSERT(ucn_cluster_epoch_compare(&a, &b) ==
                UCN_CLUSTER_EPOCH_RELATION_SAME);
    TEST_ASSERT(ucn_cluster_epoch_compare(&b, &a) ==
                UCN_CLUSTER_EPOCH_RELATION_SAME);

    /* CONFLICT is symmetric: different head, same cluster + term. */
    a.cluster_id = 3U; a.term = 7U; a.head_node_id = 1U;
    b.cluster_id = 3U; b.term = 7U; b.head_node_id = 2U;
    TEST_ASSERT(ucn_cluster_epoch_compare(&a, &b) ==
                UCN_CLUSTER_EPOCH_RELATION_CONFLICT);
    TEST_ASSERT(ucn_cluster_epoch_compare(&b, &a) ==
                UCN_CLUSTER_EPOCH_RELATION_CONFLICT);

    /* FOREIGN is symmetric and term-independent: same term, diff cluster. */
    a.cluster_id = 4U; a.term = 9U; a.head_node_id = 1U;
    b.cluster_id = 5U; b.term = 9U; b.head_node_id = 2U;
    TEST_ASSERT(ucn_cluster_epoch_compare(&a, &b) ==
                UCN_CLUSTER_EPOCH_RELATION_FOREIGN);
    TEST_ASSERT(ucn_cluster_epoch_compare(&b, &a) ==
                UCN_CLUSTER_EPOCH_RELATION_FOREIGN);

    /* A single direction rule must never hold across clusters: even when
     * b's term is far higher, the relation stays FOREIGN (domain-first). */
    a.cluster_id = 6U; a.term = 2U; a.head_node_id = 1U;
    b.cluster_id = 7U; b.term = 100U; b.head_node_id = 1U;
    TEST_ASSERT(ucn_cluster_epoch_compare(&a, &b) ==
                UCN_CLUSTER_EPOCH_RELATION_FOREIGN);
    TEST_ASSERT(ucn_cluster_epoch_compare(&b, &a) ==
                UCN_CLUSTER_EPOCH_RELATION_FOREIGN);
    return 0;
}

/* CLV2-M03 (03-09): reproducible property gate.  One fixed master seed
 * derives several independent xorshift streams, so every failure retains a
 * stable seed/case for replay while the input space is much wider than the
 * hand-picked 03-01 boundary cases. */
#define CLUSTER_EPOCH_PROPERTY_MASTER_SEED UINT32_C(0xE30C0909)
#define CLUSTER_EPOCH_PROPERTY_SEEDS ((size_t)8U)
#define CLUSTER_EPOCH_PROPERTY_CASES ((size_t)1024U)
#define CLUSTER_PROVIDER_PROPERTY_CASES ((size_t)96U)

static uint32_t cluster_test_property_rand(uint32_t *state)
{
    uint32_t value;

    if (state == NULL) {
        return 0U;
    }
    value = *state;
    if (value == 0U) {
        value = CLUSTER_EPOCH_PROPERTY_MASTER_SEED;
    }
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

static int cluster_test_epoch_property_random(void)
{
    uint32_t master = CLUSTER_EPOCH_PROPERTY_MASTER_SEED;
    size_t seed_index;

    for (seed_index = 0U; seed_index < CLUSTER_EPOCH_PROPERTY_SEEDS;
         ++seed_index) {
        uint32_t state = cluster_test_property_rand(&master);
        size_t case_index;

        for (case_index = 0U; case_index < CLUSTER_EPOCH_PROPERTY_CASES;
             ++case_index) {
            ucn_cluster_epoch_t a;
            ucn_cluster_epoch_t b;
            ucn_cluster_epoch_t c;
            ucn_cluster_epoch_relation_t ab;
            ucn_cluster_epoch_relation_t ba;
            uint32_t base_term;

            a.cluster_id = cluster_test_property_rand(&state);
            a.term = cluster_test_property_rand(&state);
            a.head_node_id = cluster_test_property_rand(&state);
            b.cluster_id = (cluster_test_property_rand(&state) & 1U) != 0U
                               ? a.cluster_id
                               : (a.cluster_id ^ UINT32_C(0x80000000));
            b.term = cluster_test_property_rand(&state);
            b.head_node_id = cluster_test_property_rand(&state);
            ab = ucn_cluster_epoch_compare(&a, &b);
            ba = ucn_cluster_epoch_compare(&b, &a);

            /* The two-way relation is fully determined by the domains and
             * fields.  In particular no random foreign high Term may become
             * a higher authority. */
            if (a.cluster_id != b.cluster_id) {
                TEST_ASSERT(ab == UCN_CLUSTER_EPOCH_RELATION_FOREIGN);
                TEST_ASSERT(ba == UCN_CLUSTER_EPOCH_RELATION_FOREIGN);
                TEST_ASSERT(ucn_cluster_epoch_is_foreign(&a, &b) == true);
                TEST_ASSERT(ucn_cluster_epoch_is_same_cluster(&a, &b) ==
                            false);
            } else if (a.term < b.term) {
                TEST_ASSERT(ab == UCN_CLUSTER_EPOCH_RELATION_LOWER);
                TEST_ASSERT(ba == UCN_CLUSTER_EPOCH_RELATION_HIGHER);
            } else if (a.term > b.term) {
                TEST_ASSERT(ab == UCN_CLUSTER_EPOCH_RELATION_HIGHER);
                TEST_ASSERT(ba == UCN_CLUSTER_EPOCH_RELATION_LOWER);
            } else if (a.head_node_id == b.head_node_id) {
                TEST_ASSERT(ab == UCN_CLUSTER_EPOCH_RELATION_SAME);
                TEST_ASSERT(ba == UCN_CLUSTER_EPOCH_RELATION_SAME);
            } else {
                TEST_ASSERT(ab == UCN_CLUSTER_EPOCH_RELATION_CONFLICT);
                TEST_ASSERT(ba == UCN_CLUSTER_EPOCH_RELATION_CONFLICT);
            }

            /* Strict Term ordering is transitive only within one Cluster
             * domain.  Use an overflow-safe three-term chain. */
            base_term = cluster_test_property_rand(&state) %
                        (UINT32_MAX - UINT32_C(2));
            a.cluster_id = cluster_test_property_rand(&state);
            a.term = base_term;
            a.head_node_id = cluster_test_property_rand(&state);
            b.cluster_id = a.cluster_id;
            b.term = base_term + 1U;
            b.head_node_id = cluster_test_property_rand(&state);
            c.cluster_id = a.cluster_id;
            c.term = base_term + 2U;
            c.head_node_id = cluster_test_property_rand(&state);
            TEST_ASSERT(ucn_cluster_epoch_compare(&a, &b) ==
                        UCN_CLUSTER_EPOCH_RELATION_LOWER);
            TEST_ASSERT(ucn_cluster_epoch_compare(&b, &c) ==
                        UCN_CLUSTER_EPOCH_RELATION_LOWER);
            TEST_ASSERT(ucn_cluster_epoch_compare(&a, &c) ==
                        UCN_CLUSTER_EPOCH_RELATION_LOWER);
        }
    }
    return 0;
}

/* The Provider is deliberately treated as untrusted input.  This property
 * runs all four equivalence classes (zero, broadcast, parent reuse, valid)
 * against many random parents and terms.  It verifies the public step path,
 * not an internal validation helper. */
static int cluster_test_provider_property_random(void)
{
    uint32_t master = UINT32_C(0x1D5AFE42);
    size_t seed_index;

    for (seed_index = 0U; seed_index < CLUSTER_EPOCH_PROPERTY_SEEDS;
         ++seed_index) {
        uint32_t state = cluster_test_property_rand(&master);
        size_t case_index;

        for (case_index = 0U; case_index < CLUSTER_PROVIDER_PROPERTY_CASES;
             ++case_index) {
            cluster_test_network_t network;
            cluster_test_node_t *node;
            ucn_cluster_t *cluster;
            ucn_cluster_config_t config;
            cluster_test_id_provider_t provider;
            uint32_t parent_cluster_id;
            uint32_t candidate_id;
            uint32_t mode;
            bool invalid;

            TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
            network.now_ms = 100U;
            node = &network.nodes[0];
            config = node->cluster.config;
            parent_cluster_id = cluster_test_property_rand(&state);
            if (parent_cluster_id == 0U ||
                parent_cluster_id == UCN_NODE_BROADCAST) {
                parent_cluster_id = UINT32_C(1);
            }
            mode = cluster_test_property_rand(&state) & UINT32_C(3);
            candidate_id = cluster_test_property_rand(&state);
            if (candidate_id == 0U || candidate_id == UCN_NODE_BROADCAST ||
                candidate_id == parent_cluster_id) {
                candidate_id ^= UINT32_C(0x5A5A5A5A);
                if (candidate_id == 0U ||
                    candidate_id == UCN_NODE_BROADCAST ||
                    candidate_id == parent_cluster_id) {
                    candidate_id = parent_cluster_id == UINT32_C(1)
                                       ? UINT32_C(2)
                                       : UINT32_C(1);
                }
            }
            if (mode == 0U) {
                candidate_id = 0U;
            } else if (mode == 1U) {
                candidate_id = UCN_NODE_BROADCAST;
            } else if (mode == 2U) {
                candidate_id = parent_cluster_id;
            }
            invalid = mode != UINT32_C(3);

            (void)memset(&provider, 0, sizeof(provider));
            provider.ids[0] = candidate_id;
            provider.supplied = 1U;
            provider.result = UCN_OK;
            config.make_cluster_id = cluster_test_make_id;
            config.cluster_id_context = &provider;
            config.cluster_id_incarnation = cluster_test_property_rand(&state);
            TEST_ASSERT(ucn_cluster_init(&node->cluster, &config) == UCN_OK);
            cluster = &node->cluster;
            cluster->last_cluster_id = parent_cluster_id;
            cluster->max_seen_term = cluster_test_property_rand(&state);
            cluster->last_stable_head = network.nodes[1].node_id;
            cluster->observation_deadline_ms = 1U;

            if (invalid) {
                TEST_ASSERT(ucn_cluster_step(cluster) == UCN_ERR_CONFIG);
                TEST_ASSERT(cluster->role == UCN_CLUSTER_ROLE_DETACHED);
                TEST_ASSERT(cluster->cluster_id == 0U && cluster->term == 0U);
                TEST_ASSERT(cluster->cluster_id_round == 0U);
            } else {
                TEST_ASSERT(ucn_cluster_step(cluster) == UCN_OK);
                TEST_ASSERT(cluster->role == UCN_CLUSTER_ROLE_CANDIDATE);
                TEST_ASSERT(cluster->cluster_id == candidate_id);
                TEST_ASSERT(cluster->term == 1U);
                TEST_ASSERT(cluster->cluster_id_round == 1U);
            }
            TEST_ASSERT(provider.count == 1U);
        }
    }
    return 0;
}

/* CLV2-M03 (03-02): ucn_cluster_active_epoch_get() is the logical
 * unification of the node's current cluster_id / term / head_node_id.
 * BEHAVIOR-EQUIVALENT: the physical storage is unchanged (the struct
 * still holds the three scalars; cluster_bytes is frozen), so the getter
 * must return EXACTLY the stored fields for every non-NULL object.
 * CONTRACT (03-02 MINOR cleanup): {0,0,0} is guaranteed for NULL only;
 * a normally initialized DETACHED node has zero fields BY INVARIANT
 * (set_detached() clears the three scalars), not by a getter-enforced
 * rule.  The staged 9/12/4 case below is a RAW PROJECTION test - it
 * proves the getter mirrors the stored scalars verbatim, it is not
 * asserting that (9,12,4) is a valid Active Epoch.  03-03+ will feed
 * this value into ucn_cluster_epoch_compare() for the Head-Offer /
 * Merge / Authority decisions. */
static int cluster_test_active_epoch_access(void)
{
    cluster_test_network_t network;
    ucn_cluster_t *c;
    ucn_cluster_epoch_t epoch;

    /* NULL -> {0,0,0}. */
    epoch = ucn_cluster_active_epoch_get(NULL);
    TEST_ASSERT(epoch.cluster_id == 0U);
    TEST_ASSERT(epoch.term == 0U);
    TEST_ASSERT(epoch.head_node_id == 0U);

    /* A fresh node is DETACHED: active epoch is {0,0,0}. */
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    c = &network.nodes[0].cluster;
    epoch = ucn_cluster_active_epoch_get(c);
    TEST_ASSERT(epoch.cluster_id == 0U);
    TEST_ASSERT(epoch.term == 0U);
    TEST_ASSERT(epoch.head_node_id == 0U);

    /* Staged cluster: getter returns exactly the stored fields. */
    c->cluster_id = 9U;
    c->term = 12U;
    c->head_node_id = 4U;
    epoch = ucn_cluster_active_epoch_get(c);
    TEST_ASSERT(epoch.cluster_id == 9U);
    TEST_ASSERT(epoch.term == 12U);
    TEST_ASSERT(epoch.head_node_id == 4U);
    return 0;
}

static int cluster_test_m06_legacy_auto_commit_bridge(void)
{
    ucn_cluster_member_t member;

    (void)memset(&member, 0xA5, sizeof(member));
    TEST_ASSERT(member_initialize_legacy(&member, 19U, 100U, 50U));
    TEST_ASSERT(member.occupied);
    TEST_ASSERT(member.status == UCN_CLUSTER_MEMBER_STATUS_COMMITTED);
    TEST_ASSERT(member.voting);
    TEST_ASSERT(member.wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V3);
    TEST_ASSERT(!member.provisional_deadline_armed);
    TEST_ASSERT(member.provisional_deadline_ms == 0U);
    TEST_ASSERT(ucn_cluster_member_record_is_valid(&member));
    return 0;
}

int test_cluster(void)
{
    /* CLV2-M03 (03-01): Epoch comparator boundary tests (pure infra). */
    TEST_ASSERT(cluster_test_epoch_comparator() == 0);
    TEST_ASSERT(cluster_test_epoch_property_sanity() == 0);
    TEST_ASSERT(cluster_test_epoch_property_random() == 0);
    TEST_ASSERT(cluster_test_provider_property_random() == 0);
    /* CLV2-M03 (03-02): active_epoch read accessor (behavior-equivalent). */
    TEST_ASSERT(cluster_test_active_epoch_access() == 0);
    /* CLV2-M06 (06-09): this assertion executes only in the ucn_tests
     * self-compiled membership copy with UCN_CLUSTER_ENABLE_TEST_HOOKS.
     * The independent membership model target proves the production archive
     * takes the opposite v3-provisional branch. */
    TEST_ASSERT(cluster_test_m06_legacy_auto_commit_bridge() == 0);
    TEST_ASSERT(cluster_test_codec_and_security() == 0);
    TEST_ASSERT(cluster_test_v3_codec() == 0);
    TEST_ASSERT(cluster_test_backup_sync() == 0);
    /* CLV2-01-04e.2: handle_backup_member_sync() SYNC_END wiring through
     * the single transition entry point (explicit SNAPSHOT_READY reason,
     * fail-closed desync gate, M01.0.2 takeover precedence). */
    TEST_ASSERT(cluster_test_backup_sync_transition() == 0);
    /* CLV2-01-04e.7: the member_sync RE-ENTRY (READY -> SYNCING,
     * RESYNC_STARTED, fail-closed on desync) and DETACH (SYNCING|READY|
     * TAKEOVER -> DETACHED_OBSERVE, PRIMARY_LOST, d.4 preflight) edges
     * now commit explicitly instead of relying on the end-of-RX mint. */
    TEST_ASSERT(cluster_test_backup_member_sync_resync_edges() == 0);
    TEST_ASSERT(cluster_test_backup_member_sync_detach() == 0);
    TEST_ASSERT(cluster_test_backup_ready_fencing() == 0);
    TEST_ASSERT(cluster_test_backup_ready_transition() == 0);
    TEST_ASSERT(cluster_test_primary_heartbeat_fencing() == 0);
    TEST_ASSERT(cluster_test_member_nonce_32bit() == 0);
    TEST_ASSERT(cluster_test_golden_trace() == 0);
    TEST_ASSERT(cluster_test_backup_epoch_fencing() == 0);
    TEST_ASSERT(cluster_test_delta_gap_resync() == 0);
    TEST_ASSERT(cluster_test_backup_reject_switches_candidate() == 0);
    TEST_ASSERT(cluster_test_join_txid_and_stepdown_nonce() == 0);
    TEST_ASSERT(cluster_test_join_reject_shadow_transition() == 0);
    TEST_ASSERT(cluster_test_join_accept_out_of_order_after_backup_assign() == 0);
    /* CLV2-01-04e.1: handle_backup_assign() commits the member/join ->
     * BACKUP_SYNCING transition (BACKUP_ASSIGNED) before the primary/
     * generation/mirror writes, fail-closed on a shadow desync. */
    TEST_ASSERT(cluster_test_backup_assign_transition() == 0);
    TEST_ASSERT(cluster_test_join_pending_stepdown() == 0);
    TEST_ASSERT(cluster_test_member_stepdown_shadow_transition() == 0);
    /* CLV2-01-04e.7 (human audit MAJOR 2.C): the BACKUP sub-branch of
     * the HEAD_STEPDOWN handler routes through the single transition
     * entry point (STEPDOWN_ORDERED) with the b.6 fail-closed nonce-fence
     * discipline - the fence is consumed only after the transition
     * succeeds, and the M01.0.2 takeover&&syncing combo stays expressible. */
    TEST_ASSERT(cluster_test_backup_stepdown_shadow_transition() == 0);
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
    /* CLV2-01-04f.5: handle_head_takeover() RECOVERY_HEAD -> MEMBER_ACTIVE
     * (RECOVERY_YIELDED, fail-closed) + the M01.0.2 BACKUP_TAKEOVER source
     * through the e.6 BACKUP branch. */
    TEST_ASSERT(cluster_test_recovery_head_takeover_wiring() == 0);

    /* CLV2-01-04f (f1): the recovery STEP sites (arm backoff / declare /
     * TTL stepdown) commit through the single transition entry point,
     * with the non-quorum re-arm phase-preserving and a fail-closed
     * shadow-desync sibling for each transition. */
    TEST_ASSERT(cluster_test_recovery_step_transitions() == 0);
    /* CLV2-01-04f.4: handle_recovery_declare() inbound edges (RECOVERY_HEAD
     * yield / plain join / BACKUP source / MEMBER self / re-declaration
     * refresh) now commit through the single transition entry point with
     * fail-closed shadow-desync siblings; handle_recovery_ack audited
     * phase-preserving. */
    TEST_ASSERT(cluster_test_recovery_declare_wiring() == 0);
    TEST_ASSERT(cluster_test_stable_switchback() == 0);
    /* CLV2-M03 (03-03): the HEAD branch of consider_head_offer()
     * classifies by Epoch relation - foreign high/low Term offers never
     * surrender (A/2 vs B/100 -> FOREIGN), same-cluster higher Term
     * yields, while same-cluster same-term CONFLICT enters the local safe
     * wait and never a foreign merge or score arbitration. */
    TEST_ASSERT(cluster_test_epoch_classified_head_offer() == 0);
    TEST_ASSERT(cluster_test_join_pending_epoch_domain_retarget() == 0);
    TEST_ASSERT(cluster_test_global_higher_authority_pre_dispatch() == 0);
    TEST_ASSERT(cluster_test_global_term_conflict_pre_dispatch() == 0);
    TEST_ASSERT(cluster_test_candidate_lower_authority_sequence() == 0);
    TEST_ASSERT(cluster_test_stepdown_term_conflict_rx() == 0);
    TEST_ASSERT(cluster_test_serial_exhaustion_fails_closed() == 0);
    TEST_ASSERT(cluster_test_cluster_id_provider() == 0);
    TEST_ASSERT(cluster_test_detach_history_rejects_old_term() == 0);
    /* CLV2-M12 (12-01): lineage capture at Member/Backup fence exits. */
    TEST_ASSERT(cluster_test_recovery_lineage_capture() == 0);
    /* CLV2-M12 (12-02): Recovery ID uniqueness + invalid-answer fail-closed. */
    TEST_ASSERT(cluster_test_recovery_id_uniqueness() == 0);
    /* CLV2-M12 (12-03): bounded exponential backoff + stable-join reset. */
    TEST_ASSERT(cluster_test_recovery_backoff_and_reset() == 0);
    /* CLV2-M12 (12-04): lineage rank comparator + DECLARE arbitration. */
    TEST_ASSERT(cluster_test_recovery_rank() == 0);
    TEST_ASSERT(cluster_test_recovery_rank_arbitration() == 0);
    /* CLV2-M12 (12-05): recovery authority scope + v3 fail-closed pins. */
    TEST_ASSERT(cluster_test_recovery_scope() == 0);
    /* CLV2-M12 (12-06): DECLARE/ACK round + lineage binding. */
    TEST_ASSERT(cluster_test_recovery_round_binding() == 0);
    /* CLV2-M12.1 (MAJOR-2): Recovery Member current-winner fencing. */
    TEST_ASSERT(cluster_test_recovery_member_winner_fence() == 0);
    /* CLV2-M12.2: accepted Recovery Heads adopt lineage and bind Term. */
    TEST_ASSERT(cluster_test_recovery_lineage_adoption_and_term_binding() == 0);
    /* CLV2-M12.3 full-audit cross-boundary closure. */
    TEST_ASSERT(cluster_test_recovery_domain_exit_and_exact_identity() == 0);
    /* CLV2-M12 (12-07): stable precedence reclaim. */
    TEST_ASSERT(cluster_test_recovery_stable_precedence() == 0);
    /* CLV2-M12 (12-08): min_recovery_peers isolation policy. */
    TEST_ASSERT(cluster_test_recovery_isolation_policy() == 0);
    /* CLV2-M12 (12-10): the composed recovery suite (Safety-4/Liveness). */
    TEST_ASSERT(cluster_test_recovery_suite_m12() == 0);
    /* CLV2-01-04f (f3 SITE A): consider_head_offer() RECOVERY_* sources
     * (RECOVERY_OBSERVE / RECOVERY_ELECTION) now commit RECOVERY_* ->
     * JOIN_PENDING through the single entry point before the join payload
     * (the legacy stable-Head offer event decides THAT the join runs;
     * cluster_transition() validates whether the shadow agrees). */
    TEST_ASSERT(cluster_test_recovery_offer_join_wiring() == 0);
    TEST_ASSERT(cluster_test_backup_challenge() == 0);
    TEST_ASSERT(cluster_test_timing_profiles() == 0);
    TEST_ASSERT(cluster_test_persistence_init_restore() == 0);
    TEST_ASSERT(cluster_test_phase_mapping_static() == 0);
    TEST_ASSERT(cluster_test_shadow_lifecycle() == 0);
    TEST_ASSERT(cluster_test_shadow_grace_timeout() == 0);
    TEST_ASSERT(cluster_test_lease_grace_reasons() == 0);
    TEST_ASSERT(cluster_test_shadow_takeover_late_sync() == 0);
    TEST_ASSERT(cluster_test_transition_matrix() == 0);
    TEST_ASSERT(cluster_test_transition_preflight() == 0);
    TEST_ASSERT(cluster_test_transition_apply() == 0);
    /* CLV2-01-04d.1/d.2: the backup-selection and sweep-done sites now
     * commit their phase transitions through the single entry point. */
    TEST_ASSERT(cluster_test_assign_backup_transition() == 0);
    TEST_ASSERT(cluster_test_backup_assignment_sweep_transition() == 0);
    TEST_ASSERT(cluster_test_election_join_and_failover() == 0);
    TEST_ASSERT(cluster_test_head_offer_join_wiring() == 0);
    TEST_ASSERT(cluster_test_member_offer_grace_refresh_wiring() == 0);
    TEST_ASSERT(cluster_test_member_same_term_conflict_blocks_score_switch() == 0);
    /* CLV2-01-04d.5/.6 + 01-04f (f3 SITE B): backup_resync()
     * (HEAD_STABLE -> HEAD_BACKUP_SYNCING, RESYNC_STARTED) and
     * begin_ordered_stepdown() (HEAD_* -> STEPPING_DOWN and RECOVERY_HEAD
     * -> STEPPING_DOWN, STEPDOWN_ORDERED) wired through the single
     * transition entry point. */
    TEST_ASSERT(cluster_test_resync_transition_wiring() == 0);
    TEST_ASSERT(cluster_test_head_stepdown_transition_wiring() == 0);
    TEST_ASSERT(cluster_test_head_ladder_closure() == 0);
    /* CLV2-01-04e: the takeover-lifecycle sites (start_takeover /
     * complete_takeover / takeover timeout / handle_head_takeover) are
     * wired through the single transition entry point. */
    TEST_ASSERT(cluster_test_takeover_lifecycle_wiring() == 0);
    /* CLV2-01-04f.2: the LAST two non-Recovery residual step-side mints -
     * the BACKUP missed-heartbeat eligible path (BACKUP_SYNCING ->
     * RECOVERY_OBSERVE, PRIMARY_LOST) and the STEPPING_DOWN deadline
     * (STEPPING_DOWN -> JOIN_PENDING, STEPDOWN_COMPLETE) - now commit
     * explicitly through the single entry point, fail closed on a shadow
     * desync, and preserve M01.0.2 (takeover-active precedence). */
    TEST_ASSERT(cluster_test_backup_miss_eligible_wiring() == 0);
    TEST_ASSERT(cluster_test_stepdown_deadline_wiring() == 0);
    TEST_ASSERT(cluster_test_capacity_is_bounded() == 0);
    TEST_ASSERT(cluster_test_neighbor_summary_api() == 0);
    TEST_ASSERT(cluster_test_remove_member_backup_loss() == 0);
    /* CLV2-01-04a review B (T-A): every phase pair actually OBSERVED by
     * the scenario suite must be a member of the observed SPEC
     * (CLUSTER_TRANSITION_OBSERVED_ALLOWED = DIRECT + tick compounds).
     * Run LAST so the collector has seen every tick. */
    TEST_ASSERT(cluster_test_observed_within_spec() == 0);
    return 0;
}
