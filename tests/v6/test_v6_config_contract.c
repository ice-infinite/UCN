#include "ucn/v6/ucn_v6_config.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static ucn_v6_feature_manifest_t header_manifest(void)
{
    ucn_v6_feature_manifest_t value;
    memset(&value, 0, sizeof(value));
    value.api_version = UCN_V6_API_VERSION;
    value.storage_layout = UCN_V6_STORAGE_LAYOUT;
    value.feature_bits = UCN_V6_COMPILED_FEATURE_BITS;
    value.layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    value.max_bindings = UCN_V6_CONFIG_MAX_BINDINGS;
    value.max_active_groups = UCN_V6_CONFIG_MAX_ACTIVE_GROUPS;
    value.bootstrap_pending = UCN_V6_CONFIG_BOOTSTRAP_PENDING;
    value.bootstrap_links = UCN_V6_CONFIG_BOOTSTRAP_LINKS;
    value.operation_slots = UCN_V6_CONFIG_OPERATION_SLOTS;
    value.operation_high_waters = UCN_V6_CONFIG_OPERATION_HIGH_WATERS;
    value.active_group_slots = UCN_V6_CONFIG_ACTIVE_GROUP_SLOTS;
    value.static_group_slots = UCN_V6_CONFIG_STATIC_GROUP_SLOTS;
    value.group_key_slots = UCN_V6_CONFIG_GROUP_KEY_SLOTS;
    value.owner_event_depth = UCN_V6_CONFIG_OWNER_EVENT_DEPTH;
    value.security_sessions = UCN_V6_CONFIG_SECURITY_SESSIONS;
    value.acl_entries = UCN_V6_CONFIG_ACL_ENTRIES;
    value.group_replay_sources = UCN_V6_CONFIG_GROUP_REPLAY_SOURCES;
    value.capability_peers = UCN_V6_CONFIG_CAPABILITY_PEERS;
    value.capability_paths = UCN_V6_CONFIG_CAPABILITY_PATHS;
    value.group_discovery_hints = UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS;
    value.group_discovery_links = UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS;
    value.route_sets = UCN_V6_CONFIG_ROUTE_SETS;
    value.route_paths_per_set = UCN_V6_CONFIG_ROUTE_PATHS_PER_SET;
    value.route_candidates = UCN_V6_CONFIG_ROUTE_CANDIDATES;
    value.route_flow_pins = UCN_V6_CONFIG_ROUTE_FLOW_PINS;
    value.metric_paths = UCN_V6_CONFIG_METRIC_PATHS;
    value.qos_q0_depth = UCN_V6_CONFIG_QOS_Q0_DEPTH;
    value.qos_q1_depth = UCN_V6_CONFIG_QOS_Q1_DEPTH;
    value.qos_q2_depth = UCN_V6_CONFIG_QOS_Q2_DEPTH;
    value.qos_q3_depth = UCN_V6_CONFIG_QOS_Q3_DEPTH;
    value.qos_flow_slots = UCN_V6_CONFIG_QOS_FLOW_SLOTS;
    value.qos_inflight = UCN_V6_CONFIG_QOS_INFLIGHT;
    value.transfer_max_class = UCN_V6_CONFIG_TRANSFER_MAX_CLASS;
    value.transfer_tx_slots = UCN_V6_CONFIG_TRANSFER_TX_SLOTS;
    value.transfer_rx_slots = UCN_V6_CONFIG_TRANSFER_RX_SLOTS;
    value.transfer_recent = UCN_V6_CONFIG_TRANSFER_RECENT;
    value.transfer_window = UCN_V6_CONFIG_TRANSFER_WINDOW;
    value.transfer_credit_links = UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS;
    value.transfer_credit_reservations =
        UCN_V6_CONFIG_TRANSFER_CREDIT_RESERVATIONS;
    value.realtime_endpoints = UCN_V6_CONFIG_REALTIME_ENDPOINTS;
    value.time_domains = UCN_V6_CONFIG_TIME_DOMAINS;
    return value;
}

int main(void)
{
    union aligned_bytes {
        uint64_t align;
        uint8_t bytes[32];
    } storage;
    ucn_v6_feature_manifest_t manifest = header_manifest();

#if defined(UCN_V6_EXPECT_MANIFEST_MISMATCH)
    CHECK(ucn_v6_manifest_validate_exact(&manifest) == UCN_V6_ERR_CONFIG);
#else
    CHECK(ucn_v6_manifest_validate_exact(&manifest) == UCN_V6_OK);
    CHECK(memcmp(&manifest, ucn_v6_compiled_manifest(),
                 sizeof(manifest)) == 0);
#endif
    CHECK(ucn_v6_storage_validate(
              storage.bytes, sizeof(storage), sizeof(storage),
              UCN_V6_STORAGE_ALIGNMENT) == UCN_V6_OK);
    CHECK(ucn_v6_storage_validate(
              storage.bytes + 1U, sizeof(storage) - 1U, 8U,
              UCN_V6_STORAGE_ALIGNMENT) == UCN_V6_ERR_CONFIG);
    CHECK(ucn_v6_storage_validate(
              storage.bytes, 7U, 8U,
              UCN_V6_STORAGE_ALIGNMENT) == UCN_V6_ERR_CONFIG);
    puts("ucn v6 config contract tests passed");
    return 0;
}
