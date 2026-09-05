#ifndef UCN_V6_MESSAGE_PRIVATE_H
#define UCN_V6_MESSAGE_PRIVATE_H

#include "ucn/v6/ucn_v6_message.h"

#define UCN_V6_OPERATION_ALLOCATOR_MAGIC UINT32_C(0x564F4136)
#define UCN_V6_OPERATION_ALLOCATOR_CANARY UINT64_C(0xB6205F9C47D183AE)
#define UCN_V6_OPERATION_JOURNAL_OBJECT_MAGIC UINT32_C(0x564F4A36)
#define UCN_V6_OPERATION_JOURNAL_CANARY UINT64_C(0x78E14B390DA6C25F)

struct ucn_v6_operation_id_allocator {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    ucn_v6_message_store_ops_t store;
    ucn_v6_callback_gate_t *callback_gate;
    ucn_v6_message_witness_t witness;
    uint64_t next_id;
    uint64_t reserved_through;
    uint32_t reservation_block_size;
    bool initialized;
    bool faulted;
    uint64_t canary;
};

struct ucn_v6_operation_journal {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    ucn_v6_operation_journal_snapshot_t committed;
    ucn_v6_message_witness_t witness;
    ucn_v6_message_store_ops_t store;
    ucn_v6_callback_gate_t *callback_gate;
    bool initialized;
    bool faulted;
    uint64_t canary;
};

#endif
