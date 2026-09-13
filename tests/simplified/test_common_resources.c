#include "internal/ucn_coordinator.h"

#include <stdio.h>

UCN_STATIC_ASSERT(sizeof(ucn_handle_t) == 12U, resource_handle_must_be_12);
UCN_STATIC_ASSERT(UCN_I_OWNER_WORK_CLASS_LIMIT == 5U,
                  resource_owner_work_classes_must_be_five);
UCN_STATIC_ASSERT(UCN_I_COORDINATOR_PENDING_LIMIT == 4U,
                  resource_coordinator_slots_must_be_four);
UCN_STATIC_ASSERT(sizeof(ucn_i_dependency_requirement_t) <= 80U,
                  resource_requirement_must_remain_small);
UCN_STATIC_ASSERT(sizeof(ucn_i_coordinator_t) <= 1024U,
                  resource_coordinator_must_remain_bounded);

int main(void)
{
    printf("V6S_COMMON_RESOURCE handle=%zu lock_ops=%zu callback_gate=%zu "
           "mailbox=%zu requirement=%zu slot=%zu coordinator=%zu "
           "work_classes=%u pending_slots=%u\n",
           sizeof(ucn_handle_t), sizeof(ucn_i_lock_ops_t),
           sizeof(ucn_i_callback_gate_t), sizeof(ucn_i_owner_mailbox_t),
           sizeof(ucn_i_dependency_requirement_t),
           sizeof(ucn_i_coordinator_slot_t), sizeof(ucn_i_coordinator_t),
           (unsigned int)UCN_I_OWNER_WORK_CLASS_LIMIT,
           (unsigned int)UCN_I_COORDINATOR_PENDING_LIMIT);
    return 0;
}
