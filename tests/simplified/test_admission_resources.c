#include "internal/ucn_admission.h"
#include "internal/ucn_capability.h"
#include "internal/ucn_identity.h"

#include <stdio.h>

#if UCN_PROFILE == UCN_PROFILE_NANO
#define ADMISSION_OWNER_LIMIT 4096U
#define IDENTITY_OWNER_LIMIT 4096U
#define CAPABILITY_OWNER_LIMIT 2048U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define ADMISSION_OWNER_LIMIT 8192U
#define IDENTITY_OWNER_LIMIT 8192U
#define CAPABILITY_OWNER_LIMIT 4096U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define ADMISSION_OWNER_LIMIT 16384U
#define IDENTITY_OWNER_LIMIT 16384U
#define CAPABILITY_OWNER_LIMIT 8192U
#else
#error "unsupported UCN_PROFILE"
#endif

static int exceeds_limit(size_t actual, size_t limit)
{
    return actual > limit;
}

int main(void)
{
    size_t total = sizeof(ucn_i_admission_owner_t) +
                   sizeof(ucn_i_identity_owner_t) +
                   sizeof(ucn_i_capability_owner_t);

    printf("profile=%u admission_owner=%zu identity_owner=%zu "
           "capability_owner=%zu total=%zu admission_slots=%u "
           "binding_slots=%u capability_peers=%u\n",
           (unsigned)UCN_PROFILE,
           sizeof(ucn_i_admission_owner_t),
           sizeof(ucn_i_identity_owner_t),
           sizeof(ucn_i_capability_owner_t), total,
           (unsigned)UCN_I_ADMISSION_PENDING_COUNT,
           (unsigned)UCN_BINDING_COUNT,
           (unsigned)UCN_I_CAPABILITY_PEER_COUNT);
    if (exceeds_limit(sizeof(ucn_i_admission_owner_t),
                      ADMISSION_OWNER_LIMIT) ||
        exceeds_limit(sizeof(ucn_i_identity_owner_t),
                      IDENTITY_OWNER_LIMIT) ||
        exceeds_limit(sizeof(ucn_i_capability_owner_t),
                      CAPABILITY_OWNER_LIMIT)) {
        return 1;
    }
    return 0;
}
