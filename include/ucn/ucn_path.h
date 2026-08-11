#ifndef UCN_PATH_H
#define UCN_PATH_H

#include "ucn/ucn_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* T22.2 keeps every installed end-to-end Path in a bounded forwarding table.
 * The identity is (owner Node, owner session, short Path ID); Destination is
 * retained as an explicit guard so a Path ID cannot be reused to redirect a
 * frame to another endpoint node. */
#ifndef UCN_MAX_PATH_FORWARD_ENTRIES
#define UCN_MAX_PATH_FORWARD_ENTRIES ((size_t)8U)
#endif

typedef char ucn_max_path_forward_entries_must_be_positive[
    UCN_MAX_PATH_FORWARD_ENTRIES > 0U ? 1 : -1];
typedef char ucn_path_mtu_field_must_cover_build_frame[
    UCN_MAX_FRAME_BYTES <= UINT16_MAX ? 1 : -1];

typedef uint32_t ucn_path_id_t;

/* Optional end-to-end Path bottleneck supplied by a provisioning controller.
 * minimum_mtu == 0 means unspecified and causes the Node to derive the local
 * failover-safe Bearer intersection.  A valid capability is always intersected
 * with the current local next-hop Bearer set; it can restrict but never widen
 * what the physical next hop can carry. */
typedef struct ucn_path_capability {
    ucn_wire_profile_t maximum_wire_profile;
    uint16_t minimum_mtu;
} ucn_path_capability_t;

typedef struct ucn_path_forward_config {
    ucn_node_id_t owner;
    ucn_session_id_t owner_session_id;
    ucn_path_id_t path_id;
    ucn_node_id_t destination;
    ucn_node_id_t next_hop;
    /* Number of Link hops from this Node to destination.  A terminal entry
     * must be zero; every forwarding entry must be non-zero. */
    uint8_t remaining_hops;
    uint8_t maximum_wire_profile;
    uint16_t minimum_mtu;
    ucn_link_t *egress_link;
    uint32_t expires_at_ms;
} ucn_path_forward_config_t;

typedef struct ucn_path_forward_entry {
    bool occupied;
    ucn_node_id_t owner;
    ucn_session_id_t owner_session_id;
    ucn_path_id_t path_id;
    ucn_node_id_t destination;
    ucn_node_id_t next_hop;
    uint8_t remaining_hops;
    uint8_t maximum_wire_profile;
    uint16_t minimum_mtu;
    ucn_link_t *egress_link;
    bool terminal;
    uint32_t expires_at_ms;
} ucn_path_forward_entry_t;

typedef struct ucn_path_stats {
    uint32_t installs;
    uint32_t revokes;
    uint32_t expired;
} ucn_path_stats_t;

typedef struct ucn_path_state {
    ucn_path_forward_entry_t entries[UCN_MAX_PATH_FORWARD_ENTRIES];
    ucn_path_stats_t stats;
} ucn_path_state_t;

bool ucn_path_is_expired(const ucn_path_forward_entry_t *entry,
                         uint32_t now_ms);
const ucn_path_forward_entry_t *ucn_path_find(
    const ucn_path_state_t *state,
    ucn_node_id_t owner,
    ucn_session_id_t owner_session_id,
    ucn_path_id_t path_id,
    ucn_node_id_t destination);
ucn_result_t ucn_path_install(ucn_path_state_t *state,
                              const ucn_path_forward_config_t *config);
ucn_result_t ucn_path_revoke(ucn_path_state_t *state,
                             ucn_node_id_t owner,
                             ucn_session_id_t owner_session_id,
                             ucn_path_id_t path_id,
                             ucn_node_id_t destination);
void ucn_path_expire(ucn_path_state_t *state, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
