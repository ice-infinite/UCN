#ifndef UCN_V6_REALTIME_PRIVATE_H
#define UCN_V6_REALTIME_PRIVATE_H

#include "ucn/v6/ucn_v6_realtime.h"

#define UCN_V6_REALTIME_OWNER_MAGIC UINT32_C(0x56365254)
#define UCN_V6_REALTIME_OWNER_SCHEMA UINT16_C(1)
#define UCN_V6_REALTIME_OWNER_CANARY UINT64_C(0x52544D45564F3641)

typedef struct ucn_v6_realtime_policy_slot {
    bool occupied;
    ucn_v6_realtime_endpoint_policy_t policy;
} ucn_v6_realtime_policy_slot_t;

typedef struct ucn_v6_time_domain_slot {
    bool occupied;
    ucn_v6_time_domain_config_t config;
    ucn_v6_time_domain_phase_t phase;
    int64_t offsets[UCN_V6_REALTIME_SAMPLE_WINDOW];
    uint8_t sample_count;
    uint8_t sample_cursor;
    uint8_t consecutive_samples;
    int64_t offset_us;
    uint64_t last_sample_local_us;
    uint64_t last_output_local_us;
    uint64_t last_output_domain_us;
    ucn_v6_route_path_ref_t route_ref;
    ucn_v6_stack_invalidation_t route_dependency;
    ucn_v6_realtime_path_proof_t path_proof;
    uint64_t dependency_deadline_us;
    uint32_t base_uncertainty_us;
    bool has_sample_high_water;
    bool has_output_high_water;
    bool dependency_invalidated;
} ucn_v6_time_domain_slot_t;

struct ucn_v6_realtime_owner {
    uint32_t magic;
    uint16_t schema;
    uint64_t layout_hash;
    const ucn_v6_route_owner_t *route_owner;
    ucn_v6_realtime_generation_store_ops_t generation_store;
    ucn_v6_callback_gate_t *callback_gate;
    ucn_v6_realtime_policy_slot_t
        policies[UCN_V6_CONFIG_REALTIME_ENDPOINTS];
    ucn_v6_time_domain_slot_t domains[UCN_V6_CONFIG_TIME_DOMAINS];
    ucn_v6_realtime_view_t stats;
    bool initialized;
    bool io_active;
    bool io_faulted;
    uint64_t canary;
};

#endif
