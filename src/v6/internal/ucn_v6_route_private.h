#ifndef UCN_V6_ROUTE_PRIVATE_H
#define UCN_V6_ROUTE_PRIVATE_H

#include "ucn/v6/ucn_v6_route.h"

#define UCN_V6_ROUTE_OWNER_MAGIC UINT32_C(0x56525445)
#define UCN_V6_ROUTE_OWNER_CANARY UINT64_C(0x7E1D4A9362B8C50F)

typedef struct ucn_v6_route_set_slot {
    bool occupied;
    ucn_v6_route_proposal_t current;
    bool previous_valid;
    ucn_v6_route_proposal_t previous;
    uint64_t previous_deadline_us;
} ucn_v6_route_set_slot_t;

typedef struct ucn_v6_route_candidate_slot {
    ucn_v6_route_candidate_view_t value;
} ucn_v6_route_candidate_slot_t;

typedef struct ucn_v6_route_flow_pin {
    bool occupied;
    ucn_v6_route_domain_t domain;
    uint32_t route_generation;
    uint64_t flow_id;
    uint16_t path_id;
    uint32_t path_generation;
    uint64_t deadline_us;
} ucn_v6_route_flow_pin_t;

typedef struct ucn_v6_route_domain_state {
    bool occupied;
    ucn_v6_route_domain_t domain;
    uint64_t candidate_transaction_high_water;
} ucn_v6_route_domain_state_t;

struct ucn_v6_route_owner {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    const ucn_v6_capability_owner_t *capability_owner;
    uint64_t candidate_timeout_us;
    uint64_t activation_retry_us;
    uint64_t previous_generation_grace_us;
    uint64_t flow_pin_lease_us;
    uint8_t activation_max_attempts;
    ucn_v6_route_domain_state_t domains[UCN_V6_CONFIG_ROUTE_SETS];
    ucn_v6_route_set_slot_t sets[UCN_V6_CONFIG_ROUTE_SETS];
    ucn_v6_route_candidate_slot_t
        candidates[UCN_V6_CONFIG_ROUTE_CANDIDATES];
    ucn_v6_route_flow_pin_t pins[UCN_V6_CONFIG_ROUTE_FLOW_PINS];
    ucn_v6_route_view_t stats;
    bool initialized;
    bool faulted;
    uint64_t canary;
};

#endif
