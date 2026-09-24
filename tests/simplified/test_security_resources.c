#include "internal/ucn_security.h"

#include <stdio.h>

#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_I_SECURITY_OWNER_LIMIT 4096U
#define UCN_I_SECURITY_WORKSPACE_LIMIT 2048U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_I_SECURITY_OWNER_LIMIT 12288U
#define UCN_I_SECURITY_WORKSPACE_LIMIT 2048U
#else
#define UCN_I_SECURITY_OWNER_LIMIT 32768U
#define UCN_I_SECURITY_WORKSPACE_LIMIT 3072U
#endif

UCN_STATIC_ASSERT(sizeof(ucn_i_security_owner_t) <=
                      UCN_I_SECURITY_OWNER_LIMIT,
                  security_owner_exceeds_profile_budget);
UCN_STATIC_ASSERT(sizeof(ucn_i_security_slot_t) <= 1536U,
                  security_slot_exceeds_fixed_budget);
UCN_STATIC_ASSERT(sizeof(ucn_i_security_packet_workspace_t) <=
                      UCN_I_SECURITY_WORKSPACE_LIMIT,
                  security_packet_workspace_exceeds_fixed_budget);
UCN_STATIC_ASSERT(sizeof(ucn_i_security_replay_handle_t) <= 64U,
                  security_replay_handle_exceeds_fixed_budget);

int main(void)
{
    printf("security_resources owner=%zu slot=%zu workspace=%zu "
           "replay_handle=%zu sessions=%u replay_reservations=%u\n",
           sizeof(ucn_i_security_owner_t),
           sizeof(ucn_i_security_slot_t),
           sizeof(ucn_i_security_packet_workspace_t),
           sizeof(ucn_i_security_replay_handle_t),
           (unsigned int)UCN_I_SECURITY_SESSION_COUNT,
           (unsigned int)UCN_I_SECURITY_REPLAY_RESERVATION_COUNT);
    return 0;
}
