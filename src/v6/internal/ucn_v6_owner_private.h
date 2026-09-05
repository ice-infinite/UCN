#ifndef UCN_V6_OWNER_PRIVATE_H
#define UCN_V6_OWNER_PRIVATE_H

#include "ucn/v6/ucn_v6_owner.h"

#define UCN_V6_OWNER_MAILBOX_MAGIC UINT32_C(0x56504F36)
#define UCN_V6_OWNER_MAILBOX_CANARY UINT64_C(0xE2146C8B3905A7DF)
#define UCN_V6_STACK_OWNER_MAGIC UINT32_C(0x56534F36)
#define UCN_V6_STACK_OWNER_CANARY UINT64_C(0xA7D1843E6C295FB0)

typedef struct ucn_v6_owner_mailbox {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    ucn_v6_owner_lock_ops_t lock_ops;
    uint16_t event_depth;
    uint16_t pending_total;
    uint16_t pending_by_event[UCN_V6_OWNER_EVENT_COUNT];
    uint8_t next_event_index;
    bool running;
    bool faulted;
    uint64_t canary;
} ucn_v6_owner_mailbox_t;

typedef struct ucn_v6_owner_mailbox_view {
    uint16_t pending_total;
    uint16_t pending_by_event[UCN_V6_OWNER_EVENT_COUNT];
    uint8_t next_event_index;
    bool running;
    bool faulted;
} ucn_v6_owner_mailbox_view_t;

struct ucn_v6_stack_owner {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    uint32_t feature_bits;
    ucn_v6_stack_hooks_t hooks;
    ucn_v6_owner_mailbox_t mailbox;
    uint64_t last_now_us;
    uint64_t next_deadline_us;
    uint32_t invalidations_applied;
    ucn_v6_result_t last_error;
    bool has_time;
    bool has_next_deadline;
    bool rerun_pending;
    bool running;
    bool initialized;
    bool faulted;
    uint64_t canary;
};

#endif
