#include "ucn/v6/ports/ucn_v6_freertos.h"
#include "ucn/v6/ucn_v6_adapter.h"
#include "ucn/v6/ucn_v6_bootstrap.h"
#include "ucn/v6/ucn_v6_capability.h"
#include "ucn/v6/ucn_v6_cluster.h"
#include "ucn/v6/ucn_v6_identity.h"
#include "ucn/v6/ucn_v6_message.h"
#include "ucn/v6/ucn_v6_owner.h"
#include "ucn/v6/ucn_v6_qos.h"
#include "ucn/v6/ucn_v6_realtime.h"
#include "ucn/v6/ucn_v6_route.h"
#if UCN_V6_FEATURE_ADAPTER_ENABLED
#include "ucn/v6/ucn_v6_runtime.h"
#endif
#include "ucn/v6/ucn_v6_security.h"
#include "ucn/v6/ucn_v6_transfer.h"

#include <stdio.h>

#define CHECK_STORAGE(type_, budget_)                                      \
    typedef char type_##_fits_public_budget[(sizeof(type_) <= (budget_)) ? \
                                                1 :                         \
                                                -1]

CHECK_STORAGE(ucn_v6_identity_authority_storage_t,
              UCN_V6_IDENTITY_AUTHORITY_STORAGE_BYTES);
CHECK_STORAGE(ucn_v6_bootstrap_owner_storage_t,
              UCN_V6_BOOTSTRAP_OWNER_STORAGE_BYTES);
CHECK_STORAGE(ucn_v6_operation_id_allocator_storage_t,
              UCN_V6_OPERATION_ID_ALLOCATOR_STORAGE_BYTES);
CHECK_STORAGE(ucn_v6_operation_journal_storage_t,
              UCN_V6_OPERATION_JOURNAL_STORAGE_BYTES);
CHECK_STORAGE(ucn_v6_stack_owner_storage_t,
              UCN_V6_STACK_OWNER_STORAGE_BYTES);
CHECK_STORAGE(ucn_v6_security_manager_storage_t,
              UCN_V6_SECURITY_MANAGER_STORAGE_BYTES);
CHECK_STORAGE(ucn_v6_capability_owner_storage_t,
              UCN_V6_CAPABILITY_OWNER_STORAGE_BYTES);
CHECK_STORAGE(ucn_v6_route_owner_storage_t,
              UCN_V6_ROUTE_OWNER_STORAGE_BYTES);
CHECK_STORAGE(ucn_v6_metric_owner_storage_t,
              UCN_V6_METRIC_OWNER_STORAGE_BYTES);
CHECK_STORAGE(ucn_v6_qos_owner_storage_t,
              UCN_V6_QOS_OWNER_STORAGE_BYTES);
CHECK_STORAGE(ucn_v6_transfer_owner_storage_t,
              UCN_V6_TRANSFER_OWNER_STORAGE_BYTES);
CHECK_STORAGE(ucn_v6_realtime_owner_storage_t,
              UCN_V6_REALTIME_OWNER_STORAGE_BYTES);
CHECK_STORAGE(ucn_v6_cluster_owner_storage_t,
              UCN_V6_CLUSTER_OWNER_STORAGE_BYTES);
CHECK_STORAGE(ucn_v6_adapter_owner_storage_t,
              UCN_V6_ADAPTER_OWNER_STORAGE_BYTES);
#if UCN_V6_FEATURE_ADAPTER_ENABLED
CHECK_STORAGE(ucn_v6_runtime_owner_storage_t,
              UCN_V6_RUNTIME_OWNER_STORAGE_BYTES);
#endif
CHECK_STORAGE(ucn_v6_freertos_port_storage_t,
              UCN_V6_FREERTOS_PORT_STORAGE_BYTES);

static void print_storage(const char *name, size_t bytes)
{
    printf("storage.%s=%zu\n", name, bytes);
}

int main(void)
{
    const ucn_v6_feature_manifest_t *manifest = ucn_v6_compiled_manifest();

    if (manifest == NULL ||
        ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK) {
        return 1;
    }

    printf("manifest.api_version=%u\n", (unsigned)manifest->api_version);
    printf("manifest.storage_layout=%u\n", (unsigned)manifest->storage_layout);
    printf("manifest.profile=%u\n", (unsigned)manifest->profile);
    printf("manifest.feature_bits=%u\n", (unsigned)manifest->feature_bits);
    printf("manifest.layout_hash=%llu\n",
           (unsigned long long)manifest->layout_hash);
    print_storage("identity_authority",
                  sizeof(ucn_v6_identity_authority_storage_t));
    print_storage("bootstrap", sizeof(ucn_v6_bootstrap_owner_storage_t));
    print_storage("operation_allocator",
                  sizeof(ucn_v6_operation_id_allocator_storage_t));
    print_storage("operation_journal",
                  sizeof(ucn_v6_operation_journal_storage_t));
    print_storage("stack_owner", sizeof(ucn_v6_stack_owner_storage_t));
    print_storage("security", sizeof(ucn_v6_security_manager_storage_t));
    print_storage("capability", sizeof(ucn_v6_capability_owner_storage_t));
    print_storage("route", sizeof(ucn_v6_route_owner_storage_t));
    print_storage("metric", sizeof(ucn_v6_metric_owner_storage_t));
    print_storage("qos", sizeof(ucn_v6_qos_owner_storage_t));
    print_storage("transfer", sizeof(ucn_v6_transfer_owner_storage_t));
    print_storage("realtime", sizeof(ucn_v6_realtime_owner_storage_t));
    print_storage("cluster", sizeof(ucn_v6_cluster_owner_storage_t));
    print_storage("adapter", sizeof(ucn_v6_adapter_owner_storage_t));
#if UCN_V6_FEATURE_ADAPTER_ENABLED
    print_storage("runtime", sizeof(ucn_v6_runtime_owner_storage_t));
#endif
    print_storage("freertos_port", sizeof(ucn_v6_freertos_port_storage_t));
    return 0;
}
