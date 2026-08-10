#include <stdint.h>
#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"
#include "ucn/ucn_path.h"
#include "ucn/ucn_policy.h"
#include "ucn/ucn_time.h"

int test_time(void)
{
    const uint32_t base_ms = UINT32_MAX - UINT32_C(5);
    uint32_t deadline;
    ucn_node_t node;
    ucn_path_state_t path_state;
    ucn_policy_state_t policy_state;
    ucn_config_t config;

    TEST_ASSERT(!ucn_duration_is_valid(0U));
    TEST_ASSERT(ucn_duration_is_valid(1U));
    TEST_ASSERT(ucn_duration_is_valid(UCN_MAX_SAFE_DURATION_MS));
    TEST_ASSERT(!ucn_duration_is_valid(UINT32_C(0x80000000)));

    deadline = ucn_deadline_from_now(UINT32_MAX - UINT32_C(5), UINT32_C(10));
    TEST_ASSERT(deadline == 4U);
    TEST_ASSERT(!ucn_deadline_expired(3U, deadline));
    TEST_ASSERT(ucn_deadline_due_within(3U, deadline, 1U));
    TEST_ASSERT(!ucn_deadline_due_within(3U, deadline, 0U));
    TEST_ASSERT(ucn_deadline_expired(4U, deadline));
    TEST_ASSERT(!ucn_deadline_due_within(4U, deadline, 1U));

    /* A natural deadline of zero is represented as one so zero can remain
     * the public no-deadline sentinel. */
    deadline = ucn_deadline_from_now(UINT32_MAX - UINT32_C(9), UINT32_C(10));
    TEST_ASSERT(deadline == 1U);
    TEST_ASSERT(!ucn_deadline_expired(0U, deadline));
    TEST_ASSERT(ucn_deadline_expired(1U, deadline));

    TEST_ASSERT(ucn_deadline_from_now(123U, 0U) == 0U);
    TEST_ASSERT(ucn_deadline_from_now(123U, UINT32_C(0x80000000)) == 0U);
    TEST_ASSERT(!ucn_deadline_expired(UINT32_MAX, 0U));

    TEST_ASSERT(!ucn_elapsed_at_least(2U, UINT32_MAX - UINT32_C(3), 7U));
    TEST_ASSERT(ucn_elapsed_at_least(3U, UINT32_MAX - UINT32_C(3), 7U));
    TEST_ASSERT(!ucn_elapsed_at_least(3U, 3U, 0U));

    (void)memset(&node, 0, sizeof(node));
    config.network_id = UINT32_C(0x12345678);
    config.node_id = UINT32_C(1);
    config.default_hop_limit = 4U;
    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    node.routes[0].valid = true;
    node.routes[0].destination = UINT32_C(2);
    node.routes[0].expires_at_ms = ucn_deadline_from_now(base_ms, 10U);
    node.path_trace_reverse[0].occupied = true;
    node.path_trace_reverse[0].expires_at_ms =
        ucn_deadline_from_now(base_ms, 10U);
    (void)ucn_node_step(&node, 3U);
    TEST_ASSERT(node.routes[0].valid);
    TEST_ASSERT(node.path_trace_reverse[0].occupied);
    (void)ucn_node_step(&node, 4U);
    TEST_ASSERT(!node.routes[0].valid);
    TEST_ASSERT(!node.path_trace_reverse[0].occupied);

    (void)memset(&path_state, 0, sizeof(path_state));
    path_state.entries[0].occupied = true;
    path_state.entries[0].expires_at_ms = ucn_deadline_from_now(base_ms, 10U);
    ucn_path_expire(&path_state, 3U);
    TEST_ASSERT(path_state.entries[0].occupied);
    ucn_path_expire(&path_state, 4U);
    TEST_ASSERT(!path_state.entries[0].occupied && path_state.stats.expired == 1U);

    (void)memset(&policy_state, 0, sizeof(policy_state));
    policy_state.flows[0].occupied = true;
    policy_state.flows[0].expires_at_ms = ucn_deadline_from_now(base_ms, 10U);
    ucn_policy_expire_flows(&policy_state, 3U);
    TEST_ASSERT(policy_state.flows[0].occupied);
    ucn_policy_expire_flows(&policy_state, 4U);
    TEST_ASSERT(!policy_state.flows[0].occupied &&
                policy_state.stats.flow_bindings_expired == 1U);
    return 0;
}
