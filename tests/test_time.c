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
    ucn_node_t step_node;
    ucn_node_t wrap_step_node;
#if UCN_FEATURE_PATH
    ucn_path_state_t path_state;
#endif
#if UCN_FEATURE_POLICY
    ucn_policy_state_t policy_state;
#endif
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

    /* The first Step establishes the observation baseline.  Exactly the
     * configured maximum is legal; only a larger gap increments violations. */
    (void)memset(&step_node, 0, sizeof(step_node));
    config.network_id = UINT32_C(0x12345678);
    config.node_id = UINT32_C(10);
    config.default_hop_limit = 4U;
    TEST_ASSERT(ucn_node_init(&step_node, &config) == UCN_OK);
    (void)ucn_node_step(&step_node, 0U);
    TEST_ASSERT(step_node.stats.last_step_ms == 0U &&
                step_node.stats.max_step_gap_ms == 0U &&
                step_node.stats.step_interval_violations == 0U);
    (void)ucn_node_step(&step_node, UCN_MAX_STEP_INTERVAL_MS);
    TEST_ASSERT(step_node.stats.last_step_ms == UCN_MAX_STEP_INTERVAL_MS &&
                step_node.stats.max_step_gap_ms == UCN_MAX_STEP_INTERVAL_MS &&
                step_node.stats.step_interval_violations == 0U);
    (void)ucn_node_step(&step_node,
                        UCN_MAX_STEP_INTERVAL_MS * UINT32_C(2) + UINT32_C(1));
    TEST_ASSERT(step_node.stats.max_step_gap_ms ==
                    UCN_MAX_STEP_INTERVAL_MS + UINT32_C(1) &&
                step_node.stats.step_interval_violations == 1U);

    /* Unsigned elapsed time keeps the same contract across millis() wrap. */
    (void)memset(&wrap_step_node, 0, sizeof(wrap_step_node));
    config.node_id = UINT32_C(11);
    TEST_ASSERT(ucn_node_init(&wrap_step_node, &config) == UCN_OK);
    (void)ucn_node_step(&wrap_step_node, UINT32_MAX - UINT32_C(3));
    (void)ucn_node_step(&wrap_step_node, UINT32_C(4));
    TEST_ASSERT(wrap_step_node.stats.last_step_ms == 4U &&
                wrap_step_node.stats.max_step_gap_ms == 8U &&
                wrap_step_node.stats.step_interval_violations == 0U);

    (void)memset(&node, 0, sizeof(node));
    config.network_id = UINT32_C(0x12345678);
    config.node_id = UINT32_C(1);
    config.default_hop_limit = 4U;
    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    node.routes[0].valid = true;
    node.routes[0].route_origin = node.config.node_id;
    node.routes[0].destination = UINT32_C(2);
    node.routes[0].expires_at_ms = ucn_deadline_from_now(base_ms, 10U);
#if UCN_FEATURE_DIAGNOSTICS
    node.path_trace_reverse[0].occupied = true;
    node.path_trace_reverse[0].expires_at_ms =
        ucn_deadline_from_now(base_ms, 10U);
#endif
    (void)ucn_node_step(&node, 3U);
    TEST_ASSERT(node.routes[0].valid);
#if UCN_FEATURE_DIAGNOSTICS
    TEST_ASSERT(node.path_trace_reverse[0].occupied);
#endif
    (void)ucn_node_step(&node, 4U);
    TEST_ASSERT(!node.routes[0].valid);
#if UCN_FEATURE_DIAGNOSTICS
    TEST_ASSERT(!node.path_trace_reverse[0].occupied);
#endif

#if UCN_FEATURE_PATH
    (void)memset(&path_state, 0, sizeof(path_state));
    path_state.entries[0].occupied = true;
    path_state.entries[0].expires_at_ms = ucn_deadline_from_now(base_ms, 10U);
    ucn_path_expire(&path_state, 3U);
    TEST_ASSERT(path_state.entries[0].occupied);
    ucn_path_expire(&path_state, 4U);
    TEST_ASSERT(!path_state.entries[0].occupied && path_state.stats.expired == 1U);
#endif

#if UCN_FEATURE_POLICY
    (void)memset(&policy_state, 0, sizeof(policy_state));
    policy_state.flows[0].occupied = true;
    policy_state.flows[0].expires_at_ms = ucn_deadline_from_now(base_ms, 10U);
    ucn_policy_expire_flows(&policy_state, 3U);
    TEST_ASSERT(policy_state.flows[0].occupied);
    ucn_policy_expire_flows(&policy_state, 4U);
    TEST_ASSERT(!policy_state.flows[0].occupied &&
                policy_state.stats.flow_bindings_expired == 1U);
#endif
    return 0;
}
