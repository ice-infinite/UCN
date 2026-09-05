#ifndef UCN_V6_CLUSTER_PRIVATE_H
#define UCN_V6_CLUSTER_PRIVATE_H

#include "ucn/v6/ucn_v6_cluster.h"

#define UCN_V6_CLUSTER_OWNER_MAGIC UINT32_C(0x5636434C)
#define UCN_V6_CLUSTER_OWNER_SCHEMA UINT16_C(1)
#define UCN_V6_CLUSTER_OWNER_CANARY UINT64_C(0x434C535456364F57)
typedef struct ucn_v6_cluster_member_slot {
    ucn_v6_cluster_member_t value;
    uint8_t capability_digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    bool keyed;
    bool fenced;
} ucn_v6_cluster_member_slot_t;

typedef struct ucn_v6_cluster_directory_slot {
    ucn_v6_cluster_directory_entry_t value;
    ucn_v6_cluster_authority_proof_view_t authority_proof;
    uint16_t ingress_link_id;
    uint32_t ingress_link_generation;
    uint32_t capability_generation;
    uint8_t capability_digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    uint64_t discovery_deadline_us;
    uint64_t capability_deadline_us;
    uint64_t local_deadline_us;
    uint32_t last_origin_sequence;
    bool keyed;
    bool fenced;
} ucn_v6_cluster_directory_slot_t;

typedef struct ucn_v6_cluster_tunnel_slot {
    ucn_v6_cluster_tunnel_t value;
    ucn_v6_stack_invalidation_t dependency;
    bool keyed;
    bool fenced;
} ucn_v6_cluster_tunnel_slot_t;

struct ucn_v6_cluster_owner {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    ucn_v6_principal_t local_principal;
    ucn_v6_binding_key_t local_binding;
    uint32_t local_session_generation;
    const ucn_v6_capability_owner_t *capability_owner;
    const ucn_v6_route_owner_t *route_owner;
    ucn_v6_cluster_authority_proof_owner_ops_t authority_proof_owner;
    ucn_v6_cluster_store_ops_t store;
    ucn_v6_callback_gate_t *callback_gate;
    ucn_v6_cluster_snapshot_t durable;
    ucn_v6_cluster_snapshot_t staging;
    uint8_t record_work[UCN_V6_CLUSTER_RECORD_BYTES];
    uint8_t record_verify[UCN_V6_CLUSTER_RECORD_BYTES];
    ucn_v6_cluster_member_slot_t members[UCN_V6_CONFIG_CLUSTER_MEMBERS];
    ucn_v6_cluster_directory_slot_t
        directory[UCN_V6_CONFIG_CLUSTER_DIRECTORY];
    ucn_v6_cluster_tunnel_slot_t tunnels[UCN_V6_CONFIG_CLUSTER_TUNNELS];
    bool observed_head_lease;
    ucn_v6_cluster_epoch_t observed_head_epoch;
    uint64_t observed_head_lease_deadline_us;
    bool authority_active;
    bool persistence_faulted;
    uint32_t persistence_commits;
    uint32_t rejected_security;
    uint32_t rejected_quorum;
    uint32_t rejected_replay;
    bool initialized;
    uint64_t canary;
};

#endif
