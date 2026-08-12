#include "test_support.h"
#include "ucn/ucn_profile.h"

int main(void)
{
    int result = 0;

    result |= test_core();
    result |= test_frame();
    result |= test_wire_profile();
    result |= test_node_wire_profile();
    result |= test_node();
    result |= test_virtual_link();
    result |= test_qos();
    result |= test_route();
    result |= test_duplicate_window();
#if UCN_FEATURE_DYNAMIC_MESH
    result |= test_aodv_lite();
    result |= test_security();
    result |= test_integration();
    result |= test_link_metrics();
    result |= test_neighbor_lifecycle();
    result |= test_hello_join();
    result |= test_neighbor_heartbeat();
    result |= test_neighbor_bearer();
    result |= test_neighbor_quality();
#if UCN_FEATURE_CANDIDATE_ROUTING
    result |= test_neighbor_route_bearer();
    result |= test_candidate_route();
#endif
    result |= test_adapter_hello();
#if UCN_FEATURE_DIAGNOSTICS
    result |= test_protocol_version();
    result |= test_path_trace();
    result |= test_node_snapshot();
    result |= test_path_control();
    result |= test_path_management_budget();
    result |= test_policy();
    result |= test_policy_diagnostic();
#endif
    result |= test_control_budget();
    result |= test_time();
    result |= test_link_contract();
    result |= test_stress();
#if UCN_PROFILE == UCN_PROFILE_FULL
    result |= test_dynamic_stress();
#endif
#endif
    result |= test_host_boundary();
    result |= test_endpoint();
    result |= test_adapter();
    result |= test_standard_adapter();
    result |= test_link_cost();
    result |= test_protocol_owner();
#if UCN_FEATURE_SERVICE
    result |= test_service();
    result |= test_service_bridge();
#endif
    result |= test_profile();
    result |= test_public_headers();
    result |= test_node_storage_header();

    if (result == 0) {
        printf("All UCN tests passed.\n");
    }

    return result;
}
