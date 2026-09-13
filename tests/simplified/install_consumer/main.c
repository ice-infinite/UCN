#include <ucn/ucn_simplified.h>
#if UCN_FEATURE_PERSISTENCE_ENABLED
#include <ucn/ucn_persistence.h>
#endif

#include "abi_contract.h"

int main(void)
{
    return sizeof(ucn_result_t) == 4U && sizeof(ucn_handle_t) == 12U &&
                   ucn_storage_required() == UCN_STORAGE_BYTES
#if UCN_FEATURE_PERSISTENCE_ENABLED
                   && ucn_persistence_storage_required() ==
                          UCN_PERSIST_STORAGE_BYTES &&
                   ucn_persist_gate_storage_required() ==
                       UCN_PERSIST_GATE_STORAGE_BYTES
#endif
               ? 0
               : 1;
}
