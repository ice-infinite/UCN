#ifndef UCN_V6_CAPABILITY_PRIVATE_H
#define UCN_V6_CAPABILITY_PRIVATE_H

#include "ucn/v6/ucn_v6_capability.h"

#define UCN_V6_CAPABILITY_OWNER_MAGIC UINT32_C(0x56434150)
#define UCN_V6_CAPABILITY_OWNER_CANARY UINT64_C(0x9D36A4F281C75BE0)

typedef struct ucn_v6_group_hint_link_budget {
    bool occupied;
    uint16_t ingress_link_id;
    uint32_t ingress_link_generation;
    uint8_t tokens;
    uint64_t last_refill_us;
} ucn_v6_group_hint_link_budget_t;

typedef struct ucn_v6_group_hint_group_budget {
    bool occupied;
    uint16_t ingress_link_id;
    uint32_t ingress_link_generation;
    uint32_t group_id;
    uint32_t group_generation;
    uint8_t tokens;
    uint64_t last_refill_us;
} ucn_v6_group_hint_group_budget_t;

struct ucn_v6_capability_owner {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    ucn_v6_capability_record_t local_record;
    uint8_t local_digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    uint64_t capability_lease_us;
    uint64_t discovery_lease_us;
    ucn_v6_cached_peer_capability_t
        peers[UCN_V6_CONFIG_CAPABILITY_PEERS];
    ucn_v6_path_capability_t paths[UCN_V6_CONFIG_CAPABILITY_PATHS];
    ucn_v6_group_discovery_hint_t
        hints[UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS];
    ucn_v6_group_hint_link_budget_t
        hint_links[UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS];
    ucn_v6_group_hint_group_budget_t
        hint_groups[UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS];
    ucn_v6_capability_view_t stats;
    bool initialized;
    bool faulted;
    uint64_t canary;
};

#endif
