#include "ucn/ucn_node.h"

#if UCN_PROFILE == UCN_PROFILE_FULL
#error "ucn_profile_stubs.c must not be compiled for the Full profile"
#endif

#if !UCN_FEATURE_DYNAMIC_MESH
ucn_result_t ucn_node_set_default_route_constraints(
    ucn_node_t *node,
    const ucn_route_constraints_t *constraints)
{
    (void)constraints;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_get_default_route_constraints(
    const ucn_node_t *node,
    ucn_route_constraints_t *constraints)
{
    (void)constraints;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_get_route_quality(const ucn_node_t *node,
                                        ucn_node_id_t destination,
                                        ucn_route_quality_t *quality)
{
    (void)destination;
    (void)quality;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}
#endif

bool ucn_path_is_expired(const ucn_path_forward_entry_t *entry,
                         uint32_t now_ms)
{
    (void)entry;
    (void)now_ms;
    return true;
}

const ucn_path_forward_entry_t *ucn_path_find(
    const ucn_path_state_t *state,
    ucn_node_id_t owner,
    ucn_session_id_t owner_session_id,
    ucn_path_id_t path_id,
    ucn_node_id_t destination)
{
    (void)state;
    (void)owner;
    (void)owner_session_id;
    (void)path_id;
    (void)destination;
    return NULL;
}

ucn_result_t ucn_path_install(ucn_path_state_t *state,
                              const ucn_path_forward_config_t *config)
{
    if (state == NULL || config == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    return UCN_ERR_CONFIG;
}

ucn_result_t ucn_path_install_capable(
    ucn_path_state_t *state,
    const ucn_path_forward_config_t *config,
    const ucn_path_capability_t *capability)
{
    (void)capability;
    if (state == NULL || config == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    return UCN_ERR_CONFIG;
}

ucn_result_t ucn_path_revoke(ucn_path_state_t *state,
                             ucn_node_id_t owner,
                             ucn_session_id_t owner_session_id,
                             ucn_path_id_t path_id,
                             ucn_node_id_t destination)
{
    (void)owner;
    (void)owner_session_id;
    (void)path_id;
    (void)destination;
    return state == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

void ucn_path_expire(ucn_path_state_t *state, uint32_t now_ms)
{
    (void)state;
    (void)now_ms;
}

bool ucn_policy_refresh_link_quality(ucn_policy_state_t *state,
                                     ucn_link_t *const *links,
                                     size_t link_count,
                                     uint32_t now_ms)
{
    (void)state;
    (void)links;
    (void)link_count;
    (void)now_ms;
    return false;
}

void ucn_policy_refresh_path_egress(ucn_policy_state_t *state,
                                    uint16_t local_path_id,
                                    ucn_link_t *active_egress_link,
                                    bool path_available)
{
    (void)state;
    (void)local_path_id;
    (void)active_egress_link;
    (void)path_available;
}

void ucn_policy_expire_flows(ucn_policy_state_t *state, uint32_t now_ms)
{
    (void)state;
    (void)now_ms;
}

void ucn_policy_mark_path_down(ucn_policy_state_t *state,
                               uint16_t local_path_id)
{
    (void)state;
    (void)local_path_id;
}

void ucn_policy_touch_q1_flow(ucn_policy_state_t *state,
                              ucn_node_id_t destination,
                              ucn_endpoint_t endpoint,
                              uint32_t now_ms)
{
    (void)state;
    (void)destination;
    (void)endpoint;
    (void)now_ms;
}

ucn_result_t ucn_node_set_node_snapshot_authorizer(
    ucn_node_t *node,
    ucn_node_snapshot_authorize_fn authorize,
    void *context)
{
    (void)authorize;
    (void)context;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_set_path_trace_authorizer(
    ucn_node_t *node,
    ucn_path_trace_authorize_fn authorize,
    void *context)
{
    (void)authorize;
    (void)context;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_set_policy_diagnostic_authorizer(
    ucn_node_t *node,
    ucn_policy_diagnostic_authorize_fn authorize,
    void *context)
{
    (void)authorize;
    (void)context;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_set_path_control_authorizer(
    ucn_node_t *node,
    ucn_path_control_authorize_fn authorize,
    void *context)
{
    (void)authorize;
    (void)context;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_set_route_policy(ucn_node_t *node,
                                       const ucn_route_policy_config_t *config)
{
    (void)config;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_clear_route_policy(ucn_node_t *node,
                                         const ucn_route_policy_key_t *key)
{
    (void)key;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

const ucn_route_policy_entry_t *ucn_node_find_route_policy(
    const ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class)
{
    (void)node;
    (void)destination;
    (void)endpoint;
    (void)traffic_class;
    return NULL;
}

ucn_result_t ucn_node_set_policy_path(ucn_node_t *node,
                                      const ucn_policy_path_config_t *config)
{
    (void)config;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_clear_policy_path(ucn_node_t *node,
                                        uint16_t local_path_id)
{
    (void)local_path_id;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

const ucn_policy_path_entry_t *ucn_node_find_policy_path(
    const ucn_node_t *node,
    uint16_t local_path_id)
{
    (void)node;
    (void)local_path_id;
    return NULL;
}

ucn_result_t ucn_node_bind_q1_flow(ucn_node_t *node,
                                   ucn_node_id_t destination,
                                   ucn_endpoint_t endpoint,
                                   uint16_t local_path_id,
                                   uint32_t lease_ms)
{
    (void)destination;
    (void)endpoint;
    (void)local_path_id;
    (void)lease_ms;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

const ucn_policy_flow_binding_t *ucn_node_find_q1_flow(
    const ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint)
{
    (void)node;
    (void)destination;
    (void)endpoint;
    return NULL;
}

const ucn_policy_link_quality_snapshot_t *ucn_node_get_link_quality(
    const ucn_node_t *node,
    const ucn_link_t *link)
{
    (void)node;
    (void)link;
    return NULL;
}

const ucn_policy_stats_t *ucn_node_get_policy_stats(const ucn_node_t *node)
{
    (void)node;
    return NULL;
}

ucn_result_t ucn_node_install_local_path(ucn_node_t *node,
                                         ucn_path_id_t path_id,
                                         ucn_node_id_t destination,
                                         ucn_node_id_t next_hop,
                                         uint8_t remaining_hops,
                                         uint32_t lease_ms)
{
    (void)path_id;
    (void)destination;
    (void)next_hop;
    (void)remaining_hops;
    (void)lease_ms;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_install_local_path_capable(
    ucn_node_t *node,
    ucn_path_id_t path_id,
    ucn_node_id_t destination,
    ucn_node_id_t next_hop,
    uint8_t remaining_hops,
    uint32_t lease_ms,
    const ucn_path_capability_t *capability)
{
    (void)path_id;
    (void)destination;
    (void)next_hop;
    (void)remaining_hops;
    (void)lease_ms;
    (void)capability;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_revoke_local_path(ucn_node_t *node,
                                        ucn_path_id_t path_id,
                                        ucn_node_id_t destination)
{
    (void)path_id;
    (void)destination;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_send_path_install(ucn_node_t *node,
                                        ucn_node_id_t control_target,
                                        ucn_path_id_t path_id,
                                        ucn_node_id_t destination,
                                        ucn_node_id_t next_hop,
                                        uint8_t remaining_hops,
                                        uint32_t lease_ms)
{
    (void)control_target;
    (void)path_id;
    (void)destination;
    (void)next_hop;
    (void)remaining_hops;
    (void)lease_ms;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_send_path_install_capable(
    ucn_node_t *node,
    ucn_node_id_t control_target,
    ucn_path_id_t path_id,
    ucn_node_id_t destination,
    ucn_node_id_t next_hop,
    uint8_t remaining_hops,
    uint32_t lease_ms,
    const ucn_path_capability_t *capability)
{
    (void)control_target;
    (void)path_id;
    (void)destination;
    (void)next_hop;
    (void)remaining_hops;
    (void)lease_ms;
    (void)capability;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_send_path_revoke(ucn_node_t *node,
                                       ucn_node_id_t control_target,
                                       ucn_path_id_t path_id,
                                       ucn_node_id_t destination)
{
    (void)control_target;
    (void)path_id;
    (void)destination;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

const ucn_path_forward_entry_t *ucn_node_find_path_forward(
    const ucn_node_t *node,
    ucn_node_id_t owner,
    ucn_session_id_t owner_session_id,
    ucn_path_id_t path_id,
    ucn_node_id_t destination)
{
    (void)node;
    (void)owner;
    (void)owner_session_id;
    (void)path_id;
    (void)destination;
    return NULL;
}

ucn_result_t ucn_node_send_path(ucn_node_t *node,
                                ucn_node_id_t destination,
                                uint8_t message_type,
                                ucn_traffic_class_t traffic_class,
                                ucn_path_id_t path_id,
                                const uint8_t *payload,
                                uint16_t payload_length)
{
    (void)destination;
    (void)message_type;
    (void)traffic_class;
    (void)path_id;
    (void)payload;
    (void)payload_length;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_request_path_trace(ucn_node_t *node,
                                         ucn_node_id_t destination,
                                         uint8_t record_limit,
                                         ucn_path_trace_handler_t handler,
                                         void *context)
{
    (void)destination;
    (void)record_limit;
    (void)handler;
    (void)context;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_request_node_snapshot(
    ucn_node_t *node,
    uint8_t result_limit,
    ucn_node_snapshot_handler_t handler,
    void *context)
{
    (void)result_limit;
    (void)handler;
    (void)context;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_request_policy_diagnostic(
    ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_policy_diagnostic_section_t section,
    uint8_t index,
    ucn_policy_diagnostic_handler_t handler,
    void *context)
{
    (void)destination;
    (void)section;
    (void)index;
    (void)handler;
    (void)context;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}
