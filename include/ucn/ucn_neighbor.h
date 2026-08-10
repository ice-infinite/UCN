#ifndef UCN_NEIGHBOR_H
#define UCN_NEIGHBOR_H

#include "ucn/ucn_link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ucn_neighbor_state {
    UCN_NEIGHBOR_EMPTY = 0,
    UCN_NEIGHBOR_CANDIDATE = 1,
    UCN_NEIGHBOR_ADMITTED = 2,
    UCN_NEIGHBOR_SUSPECT = 3,
    UCN_NEIGHBOR_REMOVED = 4,
    UCN_NEIGHBOR_REJECTED = 5,
    UCN_NEIGHBOR_EXPIRED = 6
} ucn_neighbor_state_t;

typedef enum ucn_join_policy {
    UCN_JOIN_MANUAL = 0,
    UCN_JOIN_OPEN = 1,
    UCN_JOIN_PROVIDER = 2
} ucn_join_policy_t;

/* A Neighbor represents one UCN Node ID.  A Bearer is one physical Link
 * through which that same identity was independently discovered and
 * admitted.  The bound is deliberately compile-time fixed: MCU products can
 * build it as 1 to retain the original single-Link footprint. */
#ifndef UCN_MAX_BEARERS_PER_NEIGHBOR
#define UCN_MAX_BEARERS_PER_NEIGHBOR ((size_t)2U)
#endif

typedef char ucn_max_bearers_per_neighbor_must_be_positive[
    UCN_MAX_BEARERS_PER_NEIGHBOR > 0U ? 1 : -1];
typedef char ucn_max_bearers_per_neighbor_must_fit_u8[
    UCN_MAX_BEARERS_PER_NEIGHBOR <= UINT8_MAX ? 1 : -1];

#define UCN_NEIGHBOR_PRIMARY_BEARER_NONE UINT8_MAX

typedef enum ucn_neighbor_bearer_state {
    UCN_NEIGHBOR_BEARER_EMPTY = 0,
    UCN_NEIGHBOR_BEARER_CANDIDATE = 1,
    UCN_NEIGHBOR_BEARER_ADMITTED = 2,
    UCN_NEIGHBOR_BEARER_SUSPECT = 3,
    UCN_NEIGHBOR_BEARER_DOWN = 4
} ucn_neighbor_bearer_state_t;

typedef struct ucn_neighbor_bearer {
    ucn_neighbor_bearer_state_t state;
    ucn_link_t *link;
    uint32_t last_seen_ms;
    uint32_t last_heartbeat_sent_ms;
    uint32_t quality_probe_id;
    uint32_t quality_probe_sent_at_ms;
    uint8_t quality_better_samples;
    uint8_t quality_probes_sent;
    uint8_t quality_probe_acks;
    bool heartbeat_sent;
    bool quality_probe_pending;
} ucn_neighbor_bearer_t;

typedef ucn_result_t (*ucn_neighbor_authorize_fn)(void *context,
                                                   ucn_node_id_t local_node_id,
                                                   ucn_node_id_t peer_node_id,
                                                   const ucn_link_t *link);

typedef struct ucn_neighbor_entry {
    ucn_neighbor_state_t state;
    ucn_node_id_t peer_node_id;
    uint8_t bearer_count;
    uint8_t primary_bearer_index;
    uint32_t suspect_since_ms;
    uint32_t last_bearer_quality_sample_ms;
    bool bearer_quality_sampled;
    ucn_neighbor_bearer_t bearers[UCN_MAX_BEARERS_PER_NEIGHBOR];
} ucn_neighbor_entry_t;

#ifdef __cplusplus
}
#endif

#endif
