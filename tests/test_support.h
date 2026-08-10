#ifndef UCN_TEST_SUPPORT_H
#define UCN_TEST_SUPPORT_H

#include <stdio.h>

#include "ucn/ucn_node_storage.h"

#define TEST_ASSERT(condition) \
    do { \
        if (!(condition)) { \
            printf("ASSERT failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
            return 1; \
        } \
    } while (0)

int test_core(void);
int test_frame(void);
int test_node(void);
int test_virtual_link(void);
int test_qos(void);
int test_route(void);
int test_duplicate_window(void);
int test_aodv_lite(void);
int test_security(void);
int test_host_boundary(void);
int test_integration(void);
int test_link_metrics(void);
int test_neighbor_lifecycle(void);
int test_hello_join(void);
int test_neighbor_heartbeat(void);
int test_neighbor_bearer(void);
int test_neighbor_quality(void);
int test_neighbor_route_bearer(void);
int test_endpoint(void);
int test_candidate_route(void);
int test_adapter(void);
int test_adapter_hello(void);
int test_protocol_version(void);
int test_path_trace(void);
int test_node_snapshot(void);
int test_path_control(void);
int test_path_management_budget(void);
int test_policy(void);
int test_policy_diagnostic(void);
int test_service(void);
int test_service_bridge(void);
int test_control_budget(void);
int test_time(void);
int test_link_contract(void);
int test_stress(void);
int test_dynamic_stress(void);
int test_profile(void);
int test_public_headers(void);
int test_node_storage_header(void);

#endif
