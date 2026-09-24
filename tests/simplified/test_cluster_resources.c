#include "internal/ucn_cluster.h"

#include <stdio.h>

#if UCN_PROFILE == UCN_PROFILE_NANO
#define CLUSTER_OWNER_LIMIT 8192U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define CLUSTER_OWNER_LIMIT 16384U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define CLUSTER_OWNER_LIMIT 32768U
#else
#error "unsupported UCN_PROFILE"
#endif

UCN_STATIC_ASSERT(sizeof(ucn_i_cluster_owner_t) <= CLUSTER_OWNER_LIMIT,
                  cluster_owner_exceeds_profile_budget);
UCN_STATIC_ASSERT(UCN_I_CLUSTER_RECORD_BYTES <=
                      UCN_I_CLUSTER_PERSIST_BODY_LIMIT,
                  cluster_record_exceeds_persistence_body);

int main(void)
{
    printf("profile=%u cluster_owner=%zu config_members=%u "
           "runtime_members=%u record=%u\n",
           (unsigned)UCN_PROFILE, sizeof(ucn_i_cluster_owner_t),
           (unsigned)UCN_I_CLUSTER_CONFIG_MEMBER_COUNT,
           (unsigned)UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT,
           (unsigned)UCN_I_CLUSTER_RECORD_BYTES);
    return 0;
}
