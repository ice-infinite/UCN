#include "internal/ucn_runtime.h"

#include <stdio.h>

UCN_STATIC_ASSERT(sizeof(ucn_node_t) <= UCN_STORAGE_BYTES,
                  node_fits_public_storage);
UCN_STATIC_ASSERT(sizeof(ucn_handle_t) == 12U, handle_size_is_stable);
UCN_STATIC_ASSERT(sizeof(ucn_result_t) == 4U, result_size_is_stable);

int main(void)
{
    printf("UCN_SIMPLIFIED_RESOURCE profile=%u storage=%u node=%zu "
           "adapter=%zu tx_slot=%zu tx_slots=%u endpoint=%zu path=%zu "
           "request=%zu receipt=%zu attempt=%zu buffer=%zu\n",
           (unsigned int)UCN_PROFILE, (unsigned int)UCN_STORAGE_BYTES,
           sizeof(ucn_node_t), sizeof(ucn_i_adapter_t),
           sizeof(ucn_i_tx_slot_t), (unsigned int)UCN_TX_SLOT_COUNT,
           sizeof(ucn_i_endpoint_t), sizeof(ucn_i_static_path_t),
           sizeof(ucn_i_request_t), sizeof(ucn_i_receipt_t),
           sizeof(ucn_i_attempt_t), sizeof(ucn_i_buffer_obligation_t));
    return 0;
}
