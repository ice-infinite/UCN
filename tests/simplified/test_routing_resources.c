#include "internal/ucn_flow.h"
#include "internal/ucn_route.h"

#include <stdio.h>

#if UCN_PROFILE == UCN_PROFILE_NANO
#define ROUTE_OWNER_LIMIT 4096U
#define FLOW_OWNER_LIMIT 4096U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define ROUTE_OWNER_LIMIT 12288U
#define FLOW_OWNER_LIMIT 16384U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define ROUTE_OWNER_LIMIT 32768U
#define FLOW_OWNER_LIMIT 65536U
#else
#error "unsupported UCN_PROFILE"
#endif

static int exceeds_limit(size_t actual, size_t limit)
{
    return actual > limit;
}

int main(void)
{
    printf("profile=%u route_owner=%zu flow_owner=%zu total=%zu "
           "discoveries=%u reverse=%u routes=%u static_routes=%u "
           "candidates=%u activations=%u flows=%u receipts=%u\n",
           (unsigned)UCN_PROFILE,
           sizeof(ucn_i_route_owner_t), sizeof(ucn_i_flow_owner_t),
           sizeof(ucn_i_route_owner_t) + sizeof(ucn_i_flow_owner_t),
           (unsigned)UCN_I_ROUTE_DISCOVERY_COUNT,
           (unsigned)UCN_I_ROUTE_REVERSE_COUNT,
           (unsigned)UCN_I_ROUTE_DYNAMIC_COUNT,
           (unsigned)UCN_I_ROUTE_STATIC_COUNT,
           (unsigned)UCN_I_FLOW_CANDIDATE_COUNT,
           (unsigned)UCN_I_FLOW_ACTIVATION_COUNT,
           (unsigned)UCN_I_FLOW_ACTIVE_COUNT,
           (unsigned)UCN_I_FLOW_RECEIPT_COUNT);
    if (exceeds_limit(sizeof(ucn_i_route_owner_t), ROUTE_OWNER_LIMIT) ||
        exceeds_limit(sizeof(ucn_i_flow_owner_t), FLOW_OWNER_LIMIT)) {
        return 1;
    }
    return 0;
}
