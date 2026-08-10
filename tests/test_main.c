#include "test_support.h"

int main(void)
{
    int result = 0;

    result |= test_core();
    result |= test_frame();
    result |= test_node();
    result |= test_virtual_link();
    result |= test_qos();
    result |= test_route();
    result |= test_aodv_lite();
    result |= test_security();
    result |= test_host_boundary();
    result |= test_integration();
    result |= test_link_metrics();
    result |= test_neighbor_lifecycle();
    result |= test_hello_join();
    result |= test_neighbor_heartbeat();
    result |= test_neighbor_bearer();
    result |= test_neighbor_quality();
    result |= test_neighbor_route_bearer();
    result |= test_endpoint();
    result |= test_candidate_route();
    result |= test_adapter();
    result |= test_v3();
    result |= test_path_trace();
    result |= test_node_snapshot();
    result |= test_path_control();
    result |= test_policy();
    result |= test_policy_diagnostic();
    result |= test_service();
    result |= test_service_bridge();

    if (result == 0) {
        printf("All UCN tests passed.\n");
    }

    return result;
}
