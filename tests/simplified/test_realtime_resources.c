#include "internal/ucn_realtime.h"

#include <stdio.h>

#if UCN_PROFILE == UCN_PROFILE_NANO
#define REALTIME_OWNER_LIMIT 4096U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define REALTIME_OWNER_LIMIT 8192U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define REALTIME_OWNER_LIMIT 16384U
#else
#error "unsupported UCN_PROFILE"
#endif

UCN_STATIC_ASSERT(sizeof(ucn_i_realtime_owner_t) <= REALTIME_OWNER_LIMIT,
                  realtime_owner_exceeds_profile_budget);

int main(void)
{
    printf("profile=%u realtime_owner=%zu domains=%u policies=%u sync=%u\n",
           (unsigned)UCN_PROFILE, sizeof(ucn_i_realtime_owner_t),
           (unsigned)UCN_I_REALTIME_DOMAIN_COUNT,
           (unsigned)UCN_I_REALTIME_POLICY_COUNT,
           (unsigned)UCN_I_REALTIME_SYNC_COUNT);
    return 0;
}
