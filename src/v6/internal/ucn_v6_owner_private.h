#ifndef UCN_V6_OWNER_PRIVATE_H
#define UCN_V6_OWNER_PRIVATE_H

#include "ucn/v6/ucn_v6_owner.h"

#define UCN_V6_PROTOCOL_OWNER_MAGIC UINT32_C(0x56504F36)
#define UCN_V6_PROTOCOL_OWNER_CANARY UINT64_C(0xE2146C8B3905A7DF)

struct ucn_v6_protocol_owner {
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
};

#endif
