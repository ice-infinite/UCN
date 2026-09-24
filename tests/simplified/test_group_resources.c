#include "internal/ucn_group.h"

#include <stdio.h>

#if UCN_PROFILE == UCN_PROFILE_NANO
#define GROUP_OWNER_LIMIT 8192U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define GROUP_OWNER_LIMIT 32768U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define GROUP_OWNER_LIMIT 65536U
#else
#error "unsupported UCN_PROFILE"
#endif

UCN_STATIC_ASSERT(sizeof(ucn_i_group_owner_t) <= GROUP_OWNER_LIMIT,
                  group_owner_exceeds_profile_budget);
UCN_STATIC_ASSERT(UCN_I_GROUP_RECORD_BYTES <=
                      UCN_I_GROUP_PERSIST_BODY_LIMIT,
                  group_record_exceeds_persistence_body);

int main(void)
{
    printf("profile=%u group_owner=%zu contexts=%u members=%u sends=%u "
           "attempts=%u receipts=%u record=%u\n",
           (unsigned)UCN_PROFILE, sizeof(ucn_i_group_owner_t),
           (unsigned)UCN_I_GROUP_CONTEXT_COUNT,
           (unsigned)UCN_I_GROUP_MEMBER_COUNT,
           (unsigned)UCN_I_GROUP_SEND_COUNT,
           (unsigned)UCN_I_GROUP_ATTEMPT_COUNT,
           (unsigned)UCN_I_GROUP_RX_COUNT,
           (unsigned)UCN_I_GROUP_RECORD_BYTES);
    return 0;
}
