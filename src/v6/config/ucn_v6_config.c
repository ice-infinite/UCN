#include "ucn/v6/ucn_v6_config.h"

#include <stdint.h>

static const ucn_v6_feature_manifest_t compiled_manifest = {
    UCN_V6_API_VERSION,
    UCN_V6_STORAGE_LAYOUT,
    UCN_V6_COMPILED_FEATURE_BITS,
    UCN_V6_COMPILED_LAYOUT_HASH,
    UCN_V6_CONFIG_MAX_BINDINGS,
    UCN_V6_CONFIG_MAX_ACTIVE_GROUPS,
    UCN_V6_CONFIG_BOOTSTRAP_PENDING,
    UCN_V6_CONFIG_BOOTSTRAP_LINKS,
    UCN_V6_CONFIG_OPERATION_SLOTS,
    UCN_V6_CONFIG_OPERATION_HIGH_WATERS,
    UCN_V6_CONFIG_ACTIVE_GROUP_SLOTS,
    UCN_V6_CONFIG_STATIC_GROUP_SLOTS,
    UCN_V6_CONFIG_GROUP_KEY_SLOTS,
    UCN_V6_CONFIG_OWNER_EVENT_DEPTH,
    UCN_V6_CONFIG_SECURITY_SESSIONS,
    UCN_V6_CONFIG_ACL_ENTRIES,
    UCN_V6_CONFIG_GROUP_REPLAY_SOURCES,
    UCN_V6_CONFIG_CAPABILITY_PEERS,
    UCN_V6_CONFIG_CAPABILITY_PATHS,
    UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS,
    UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS,
    UCN_V6_CONFIG_ROUTE_SETS,
    UCN_V6_CONFIG_ROUTE_PATHS_PER_SET,
    UCN_V6_CONFIG_ROUTE_CANDIDATES,
    UCN_V6_CONFIG_ROUTE_FLOW_PINS,
    UCN_V6_CONFIG_METRIC_PATHS,
    UCN_V6_CONFIG_QOS_Q0_DEPTH,
    UCN_V6_CONFIG_QOS_Q1_DEPTH,
    UCN_V6_CONFIG_QOS_Q2_DEPTH,
    UCN_V6_CONFIG_QOS_Q3_DEPTH,
    UCN_V6_CONFIG_QOS_FLOW_SLOTS,
    UCN_V6_CONFIG_QOS_INFLIGHT,
    UCN_V6_CONFIG_TRANSFER_MAX_CLASS,
    UCN_V6_CONFIG_TRANSFER_TX_SLOTS,
    UCN_V6_CONFIG_TRANSFER_RX_SLOTS,
    UCN_V6_CONFIG_TRANSFER_RECENT,
    UCN_V6_CONFIG_TRANSFER_WINDOW,
    UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS,
    UCN_V6_CONFIG_TRANSFER_CREDIT_RESERVATIONS
};

const ucn_v6_feature_manifest_t *ucn_v6_compiled_manifest(void)
{
    return &compiled_manifest;
}

ucn_v6_result_t ucn_v6_manifest_validate_exact(
    const ucn_v6_feature_manifest_t *manifest)
{
    const ucn_v6_feature_manifest_t *expected = &compiled_manifest;

    if (manifest == NULL) {
        return UCN_V6_ERR_CONFIG;
    }
    return manifest->api_version == expected->api_version &&
           manifest->storage_layout == expected->storage_layout &&
           manifest->feature_bits == expected->feature_bits &&
           manifest->layout_hash == expected->layout_hash &&
           manifest->max_bindings == expected->max_bindings &&
           manifest->max_active_groups == expected->max_active_groups &&
           manifest->bootstrap_pending == expected->bootstrap_pending &&
           manifest->bootstrap_links == expected->bootstrap_links &&
           manifest->operation_slots == expected->operation_slots &&
           manifest->operation_high_waters ==
               expected->operation_high_waters &&
           manifest->active_group_slots == expected->active_group_slots &&
           manifest->static_group_slots == expected->static_group_slots &&
           manifest->group_key_slots == expected->group_key_slots &&
           manifest->owner_event_depth == expected->owner_event_depth &&
           manifest->security_sessions == expected->security_sessions &&
           manifest->acl_entries == expected->acl_entries &&
           manifest->group_replay_sources ==
               expected->group_replay_sources &&
           manifest->capability_peers == expected->capability_peers &&
           manifest->capability_paths == expected->capability_paths &&
           manifest->group_discovery_hints ==
               expected->group_discovery_hints &&
           manifest->group_discovery_links ==
               expected->group_discovery_links &&
           manifest->route_sets == expected->route_sets &&
           manifest->route_paths_per_set == expected->route_paths_per_set &&
           manifest->route_candidates == expected->route_candidates &&
           manifest->route_flow_pins == expected->route_flow_pins &&
           manifest->metric_paths == expected->metric_paths &&
           manifest->qos_q0_depth == expected->qos_q0_depth &&
           manifest->qos_q1_depth == expected->qos_q1_depth &&
           manifest->qos_q2_depth == expected->qos_q2_depth &&
           manifest->qos_q3_depth == expected->qos_q3_depth &&
           manifest->qos_flow_slots == expected->qos_flow_slots &&
           manifest->qos_inflight == expected->qos_inflight &&
           manifest->transfer_max_class == expected->transfer_max_class &&
           manifest->transfer_tx_slots == expected->transfer_tx_slots &&
           manifest->transfer_rx_slots == expected->transfer_rx_slots &&
           manifest->transfer_recent == expected->transfer_recent &&
           manifest->transfer_window == expected->transfer_window &&
           manifest->transfer_credit_links ==
               expected->transfer_credit_links &&
           manifest->transfer_credit_reservations ==
               expected->transfer_credit_reservations ?
               UCN_V6_OK : UCN_V6_ERR_CONFIG;
}

ucn_v6_result_t ucn_v6_storage_validate(
    const void *storage,
    size_t storage_bytes,
    size_t required_bytes,
    size_t required_alignment)
{
    if (storage == NULL || required_bytes == 0U ||
        required_alignment == 0U ||
        (required_alignment & (required_alignment - 1U)) != 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (storage_bytes < required_bytes ||
        ((uintptr_t)storage & (required_alignment - 1U)) != 0U) {
        return UCN_V6_ERR_CONFIG;
    }
    return UCN_V6_OK;
}
