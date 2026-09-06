#ifndef UCN_V6_SECURITY_PRIVATE_H
#define UCN_V6_SECURITY_PRIVATE_H

#include "ucn/v6/ucn_v6_security.h"

#define UCN_V6_SECURITY_MANAGER_MAGIC UINT32_C(0x564D5336)
#define UCN_V6_SECURITY_MANAGER_CANARY UINT64_C(0x6A1F90C3E57B248D)

struct ucn_v6_security_manager {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    ucn_v6_security_snapshot_t committed;
    /* Fixed-capacity transactional scratch; never placed on an RTOS task
     * stack. / 固定容量事务暂存区，绝不放入 RTOS 任务栈。 */
    ucn_v6_security_snapshot_t staging;
    ucn_v6_security_snapshot_t verify;
    ucn_v6_security_store_ops_t store;
    ucn_v6_security_crypto_ops_t crypto;
    ucn_v6_callback_gate_t *callback_gate;
    ucn_v6_stack_invalidation_t
        invalidations[UCN_V6_SECURITY_INVALIDATION_DEPTH];
    uint16_t invalidation_head;
    uint16_t invalidation_count;
    bool initialized;
    bool faulted;
    uint64_t canary;
};

#endif
