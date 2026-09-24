#include "internal/ucn_transport.h"

#include <stdio.h>

#if UCN_PROFILE == UCN_PROFILE_NANO
#define TRANSPORT_OWNER_LIMIT 8192U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define TRANSPORT_OWNER_LIMIT 24576U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define TRANSPORT_OWNER_LIMIT 65536U
#else
#error "unsupported UCN_PROFILE"
#endif

UCN_STATIC_ASSERT(sizeof(ucn_i_transport_owner_t) <= TRANSPORT_OWNER_LIMIT,
                  transport_owner_exceeds_profile_budget);
UCN_STATIC_ASSERT(sizeof(ucn_i_transport_parent_record_t) <= 512U,
                  transport_parent_record_exceeds_budget);

int main(void)
{
    printf("profile=%u transport_owner=%zu parent_record=%zu "
           "reliable_tx=%u reliable_rx=%u reliable_bytes=%u "
           "transfer_tx=%u transfer_rx=%u transfer_bytes=%u "
           "transfer_fragments=%u parents=%u\n",
           (unsigned)UCN_PROFILE,
           sizeof(ucn_i_transport_owner_t),
           sizeof(ucn_i_transport_parent_record_t),
           (unsigned)UCN_I_TRANSPORT_RELIABLE_TX_COUNT,
           (unsigned)UCN_I_TRANSPORT_RELIABLE_RX_COUNT,
           (unsigned)UCN_I_TRANSPORT_RELIABLE_BYTES,
           (unsigned)UCN_I_TRANSPORT_TRANSFER_TX_COUNT,
           (unsigned)UCN_I_TRANSPORT_TRANSFER_RX_COUNT,
           (unsigned)UCN_I_TRANSPORT_TRANSFER_BYTES,
           (unsigned)UCN_I_TRANSPORT_TRANSFER_FRAGMENTS,
           (unsigned)UCN_I_TRANSPORT_PARENT_COUNT);
    return 0;
}
