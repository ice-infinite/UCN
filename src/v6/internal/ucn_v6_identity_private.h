#ifndef UCN_V6_IDENTITY_PRIVATE_H
#define UCN_V6_IDENTITY_PRIVATE_H

#include "ucn/v6/ucn_v6_identity.h"

#define UCN_V6_IDENTITY_AUTHORITY_MAGIC UINT32_C(0x56494136)
#define UCN_V6_IDENTITY_AUTHORITY_CANARY UINT64_C(0x91A6C35D72B40EF8)

struct ucn_v6_identity_authority {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    uint64_t record_generation;
    uint32_t realm_id;
    ucn_v6_authority_epoch_t epoch;
    uint64_t local_lease_deadline_us;
    ucn_v6_binding_slot_t bindings[UCN_V6_MAX_BINDING_SLOTS];
    ucn_v6_group_allocator_t groups;
    ucn_v6_identity_store_ops_t store;
    ucn_v6_callback_gate_t *callback_gate;
    bool epoch_valid;
    bool faulted;
    uint64_t canary;
};

#endif
