#include "ucn/ucn_adapter.h"
#include "ucn/ucn_node_storage.h"
#include "ucn/ucn_service_bridge.h"

#define CONFIG_ASSERT(name, condition) \
    typedef char name[(condition) ? 1 : -1]

CONFIG_ASSERT(config_protocol_version, UCN_PROTOCOL_VERSION == 5U);

#ifdef UCN_TEST_EXPECT_USER_OVERRIDES
CONFIG_ASSERT(config_override_frame, UCN_MAX_FRAME_BYTES == 128U);
CONFIG_ASSERT(config_override_payload, UCN_MAX_PAYLOAD_BYTES == 64U);
CONFIG_ASSERT(config_override_hops, UCN_MAX_HOPS == 8U);
CONFIG_ASSERT(config_override_adapter_queue, UCN_ADAPTER_RX_QUEUE_DEPTH == 3U);
CONFIG_ASSERT(config_override_links, UCN_MAX_LINKS == 3U);
CONFIG_ASSERT(config_override_q0, UCN_TX_Q0_DEPTH == 2U);
CONFIG_ASSERT(config_override_neighbors, UCN_MAX_NEIGHBORS == 4U);
CONFIG_ASSERT(config_override_bearers, UCN_MAX_BEARERS_PER_NEIGHBOR == 1U);
CONFIG_ASSERT(config_override_bindings, UCN_SERVICE_MAX_BINDINGS == 4U);
CONFIG_ASSERT(config_override_q0_bindings, UCN_SERVICE_MAX_Q0_BINDINGS == 1U);
CONFIG_ASSERT(config_override_q1_bindings, UCN_SERVICE_MAX_Q1_BINDINGS == 3U);
CONFIG_ASSERT(config_override_validators,
              UCN_SERVICE_BRIDGE_MAX_VALIDATORS == 1U);
/* An omitted product value is still supplied by the global defaults. */
CONFIG_ASSERT(config_override_keeps_default_q1, UCN_TX_Q1_DEPTH == 4U);
CONFIG_ASSERT(config_override_keeps_default_routes, UCN_MAX_ROUTES == 8U);
#else
/* This same contract is compiled once with global defaults and once with
 * UCN_CONFIG_NO_DEFAULTS, where only the original per-header fallbacks apply. */
CONFIG_ASSERT(config_default_profile, UCN_PROFILE == UCN_PROFILE_FULL);
CONFIG_ASSERT(config_default_service, UCN_FEATURE_SERVICE == 1);
CONFIG_ASSERT(config_default_frame, UCN_MAX_FRAME_BYTES == 256U);
CONFIG_ASSERT(config_default_payload, UCN_MAX_PAYLOAD_BYTES == 224U);
CONFIG_ASSERT(config_default_hops, UCN_MAX_HOPS == 16U);
CONFIG_ASSERT(config_default_adapter_queue, UCN_ADAPTER_RX_QUEUE_DEPTH == 2U);
CONFIG_ASSERT(config_default_physical_address,
              UCN_ADAPTER_PHYSICAL_ADDRESS_MAX == 8U);
CONFIG_ASSERT(config_default_security_required,
              UCN_SECURITY_REQUIRED_BY_DEFAULT == 0);
CONFIG_ASSERT(config_default_links, UCN_MAX_LINKS == 4U);
CONFIG_ASSERT(config_default_q0, UCN_TX_Q0_DEPTH == 4U);
CONFIG_ASSERT(config_default_q1, UCN_TX_Q1_DEPTH == 4U);
CONFIG_ASSERT(config_default_business_burst,
              UCN_BUSINESS_TX_BURST_BEFORE_MAINTENANCE == 4U);
CONFIG_ASSERT(config_default_step, UCN_MAX_STEP_INTERVAL_MS == 10U);
CONFIG_ASSERT(config_default_q0_retries,
              UCN_Q0_BACKPRESSURE_MAX_RETRIES == 3U);
CONFIG_ASSERT(config_default_routes, UCN_MAX_ROUTES == 8U);
CONFIG_ASSERT(config_default_duplicate_sources,
              UCN_DUPLICATE_SOURCE_WINDOWS == 32U);
CONFIG_ASSERT(config_default_duplicate_bits, UCN_DUPLICATE_WINDOW_BITS == 64U);
CONFIG_ASSERT(config_default_rreq_cache, UCN_RREQ_CACHE_SIZE == 16U);
CONFIG_ASSERT(config_default_discoveries, UCN_MAX_ROUTE_DISCOVERIES == 4U);
CONFIG_ASSERT(config_default_candidates, UCN_MAX_CANDIDATE_ROUTES == 8U);
CONFIG_ASSERT(config_default_route_lifetime,
              UCN_ROUTE_ENTRY_LIFETIME_MS == 30000U);
CONFIG_ASSERT(config_default_control_tokens, UCN_CONTROL_TOKEN_BURST == 4U);
CONFIG_ASSERT(config_default_pending_q1, UCN_PENDING_Q1_DEPTH == 4U);
CONFIG_ASSERT(config_default_path_trace, UCN_PATH_TRACE_PENDING_DEPTH == 2U);
CONFIG_ASSERT(config_default_snapshot, UCN_NODE_SNAPSHOT_MAX_RESULTS == 8U);
CONFIG_ASSERT(config_default_policy_diagnostic,
              UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH == 2U);
CONFIG_ASSERT(config_default_neighbors, UCN_MAX_NEIGHBORS == 8U);
CONFIG_ASSERT(config_default_bearers, UCN_MAX_BEARERS_PER_NEIGHBOR == 2U);
CONFIG_ASSERT(config_default_heartbeat, UCN_HEARTBEAT_INTERVAL_MS == 1000U);
CONFIG_ASSERT(config_default_fast_heartbeat,
              UCN_LINK_LIVENESS_FAST_HEARTBEAT_INTERVAL_MS == 250U);
CONFIG_ASSERT(config_default_fast_suspect,
              UCN_LINK_LIVENESS_FAST_SUSPECT_TIMEOUT_MS == 1250U);
CONFIG_ASSERT(config_default_fast_remove,
              UCN_LINK_LIVENESS_FAST_REMOVE_TIMEOUT_MS == 2000U);
CONFIG_ASSERT(config_default_endpoint_handlers, UCN_MAX_ENDPOINT_HANDLERS == 8U);
CONFIG_ASSERT(config_default_endpoint_security,
              UCN_MAX_ENDPOINT_SECURITY_POLICIES == 8U);
CONFIG_ASSERT(config_default_path_entries, UCN_MAX_PATH_FORWARD_ENTRIES == 8U);
CONFIG_ASSERT(config_default_policies, UCN_MAX_ROUTE_POLICIES == 8U);
CONFIG_ASSERT(config_default_policy_paths, UCN_MAX_POLICY_PATHS == 8U);
CONFIG_ASSERT(config_default_policy_flows, UCN_MAX_POLICY_FLOWS == 8U);
CONFIG_ASSERT(config_default_service_bindings, UCN_SERVICE_MAX_BINDINGS == 6U);
CONFIG_ASSERT(config_default_service_payload,
              UCN_SERVICE_MAX_PAYLOAD_BYTES == 32U);
CONFIG_ASSERT(config_default_service_q0, UCN_SERVICE_REMOTE_TX_Q0_DEPTH == 4U);
CONFIG_ASSERT(config_default_service_q1, UCN_SERVICE_REMOTE_TX_Q1_DEPTH == 4U);
CONFIG_ASSERT(config_default_validators,
              UCN_SERVICE_BRIDGE_MAX_VALIDATORS == 2U);
CONFIG_ASSERT(config_default_replay, UCN_SERVICE_BRIDGE_REPLAY_DEPTH == 4U);
#endif

int main(void)
{
    return 0;
}
