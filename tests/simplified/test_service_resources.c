#include "internal/ucn_service.h"

#include <stdio.h>

#if UCN_PROFILE == UCN_PROFILE_NANO
#define SERVICE_OWNER_LIMIT 4096U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define SERVICE_OWNER_LIMIT 16384U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define SERVICE_OWNER_LIMIT 65536U
#else
#error "unsupported UCN_PROFILE"
#endif

UCN_STATIC_ASSERT(sizeof(ucn_i_service_owner_t) <= SERVICE_OWNER_LIMIT,
                  service_owner_exceeds_profile_budget);

int main(void)
{
    printf("profile=%u service_owner=%zu requests=%u receipts=%u qos=%u "
           "operations=%u result_bytes=%u operation_body=%u\n",
           (unsigned)UCN_PROFILE, sizeof(ucn_i_service_owner_t),
           (unsigned)UCN_I_SERVICE_REQUEST_COUNT,
           (unsigned)UCN_I_SERVICE_RECEIPT_COUNT,
           (unsigned)UCN_I_SERVICE_QOS_COUNT,
           (unsigned)UCN_I_SERVICE_OPERATION_COUNT,
           (unsigned)UCN_I_SERVICE_RESULT_BYTES,
           (unsigned)UCN_I_SERVICE_OPERATION_BODY_BYTES);
    return 0;
}
