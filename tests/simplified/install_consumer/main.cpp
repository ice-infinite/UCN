#include <ucn/ucn_simplified.h>
#if UCN_FEATURE_PERSISTENCE_ENABLED
#include <ucn/ucn_persistence.h>
#endif

#include "abi_contract.h"

#include <type_traits>

static_assert(std::is_standard_layout<ucn_handle_t>::value, "handle ABI");
static_assert(std::is_standard_layout<ucn_config_t>::value, "config ABI");
static_assert(std::is_standard_layout<ucn_ports_t>::value, "ports ABI");
static_assert(std::is_standard_layout<ucn_send_options_t>::value,
              "send options ABI");
#if UCN_FEATURE_PERSISTENCE_ENABLED
static_assert(std::is_standard_layout<ucn_persist_manifest_t>::value,
              "persistence manifest ABI");
static_assert(std::is_standard_layout<ucn_persist_domain_binding_t>::value,
              "persistence domain binding ABI");
#endif

int main()
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
