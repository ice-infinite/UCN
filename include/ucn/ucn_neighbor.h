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

typedef ucn_result_t (*ucn_neighbor_authorize_fn)(void *context,
                                                   ucn_node_id_t local_node_id,
                                                   ucn_node_id_t peer_node_id,
                                                   const ucn_link_t *link);

typedef struct ucn_neighbor_entry {
    ucn_neighbor_state_t state;
    ucn_node_id_t peer_node_id;
    ucn_link_t *link;
    uint32_t last_seen_ms;
    uint32_t last_heartbeat_sent_ms;
    uint32_t suspect_since_ms;
    bool heartbeat_sent;
} ucn_neighbor_entry_t;

#ifdef __cplusplus
}
#endif

#endif
