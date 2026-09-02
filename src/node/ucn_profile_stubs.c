#include "ucn/ucn_node.h"

#if UCN_PROFILE == UCN_PROFILE_FULL
#error "ucn_profile_stubs.c must not be compiled for the Full profile"
#endif

#if !UCN_FEATURE_DYNAMIC_MESH
/*
 * EN: Validates and sets `default_route_constraints` in Profile compatibility Stub state.
 * 中文：验证并设置 Profile 兼容 Stub 状态中的 `default_route_constraints`。
 */
ucn_result_t ucn_node_set_default_route_constraints(
    ucn_node_t *node,
    const ucn_route_constraints_t *constraints)
{
    (void)constraints;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

/*
 * EN: Returns the current `default_route_constraints` view from Profile compatibility Stub state.
 * 中文：从 Profile 兼容 Stub 状态返回当前 `default_route_constraints` 视图。
 */
ucn_result_t ucn_node_get_default_route_constraints(
    const ucn_node_t *node,
    ucn_route_constraints_t *constraints)
{
    (void)constraints;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

/*
 * EN: Returns the current `route_quality` view from Profile compatibility Stub state.
 * 中文：从 Profile 兼容 Stub 状态返回当前 `route_quality` 视图。
 */
ucn_result_t ucn_node_get_route_quality(const ucn_node_t *node,
                                        ucn_node_id_t destination,
                                        ucn_route_quality_t *quality)
{
    (void)destination;
    (void)quality;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}
#endif

/*
 * EN: Checks the `is_expired` predicate against current Profile compatibility Stub state.
 * 中文：根据当前 Profile 兼容 Stub 状态检查 `is_expired` 条件。
 */
bool ucn_path_is_expired(const ucn_path_forward_entry_t *entry,
                         uint32_t now_ms)
{
    (void)entry;
    (void)now_ms;
    return true;
}

/*
 * EN: Looks up `find` in bounded Profile compatibility Stub state without allocation.
 * 中文：在固定容量的 Profile 兼容 Stub 状态中查找 `find`，且不进行动态分配。
 */
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

/*
 * EN: Validates and installs `install` in bounded Profile compatibility Stub state.
 * 中文：验证 `install` 并将其安装到固定容量的 Profile 兼容 Stub 状态中。
 */
ucn_result_t ucn_path_install(ucn_path_state_t *state,
                              const ucn_path_forward_config_t *config)
{
    if (state == NULL || config == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    return UCN_ERR_CONFIG;
}

/*
 * EN: Validates and installs `install_capable` into bounded Profile compatibility Stub state.
 * 中文：验证 `install_capable` 并将其安装到固定容量的 Profile 兼容 Stub 状态中。
 */
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

/*
 * EN: Clears or releases `revoke` from bounded Profile compatibility Stub state.
 * 中文：从固定容量的 Profile 兼容 Stub 状态中清除或释放 `revoke`。
 */
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

/*
 * EN: Checks or removes expired `expire` state in Profile compatibility Stub.
 * 中文：检查或移除 Profile 兼容 Stub 中已过期的 `expire` 状态。
 */
void ucn_path_expire(ucn_path_state_t *state, uint32_t now_ms)
{
    (void)state;
    (void)now_ms;
}

/*
 * EN: Updates `refresh_link_quality` in bounded Profile compatibility Stub state.
 * 中文：更新固定容量 Profile 兼容 Stub 状态中的 `refresh_link_quality`。
 */
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

/*
 * EN: Updates `refresh_path_egress` in bounded Profile compatibility Stub state.
 * 中文：更新固定容量 Profile 兼容 Stub 状态中的 `refresh_path_egress`。
 */
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

/*
 * EN: Checks or removes expired `expire_flows` state in Profile compatibility Stub.
 * 中文：检查或移除 Profile 兼容 Stub 中已过期的 `expire_flows` 状态。
 */
void ucn_policy_expire_flows(ucn_policy_state_t *state, uint32_t now_ms)
{
    (void)state;
    (void)now_ms;
}

/*
 * EN: Updates `mark_path_down` in bounded Profile compatibility Stub state.
 * 中文：更新固定容量 Profile 兼容 Stub 状态中的 `mark_path_down`。
 */
void ucn_policy_mark_path_down(ucn_policy_state_t *state,
                               uint16_t local_path_id)
{
    (void)state;
    (void)local_path_id;
}

/*
 * EN: Records `touch_q1_flow` in bounded Profile compatibility Stub state or statistics.
 * 中文：在固定容量的 Profile 兼容 Stub 状态或统计中记录 `touch_q1_flow`。
 */
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

/*
 * EN: Validates and sets `node_snapshot_authorizer` in Profile compatibility Stub state.
 * 中文：验证并设置 Profile 兼容 Stub 状态中的 `node_snapshot_authorizer`。
 */
ucn_result_t ucn_node_set_node_snapshot_authorizer(
    ucn_node_t *node,
    ucn_node_snapshot_authorize_fn authorize,
    void *context)
{
    (void)authorize;
    (void)context;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

/*
 * EN: Validates and sets `path_trace_authorizer` in Profile compatibility Stub state.
 * 中文：验证并设置 Profile 兼容 Stub 状态中的 `path_trace_authorizer`。
 */
ucn_result_t ucn_node_set_path_trace_authorizer(
    ucn_node_t *node,
    ucn_path_trace_authorize_fn authorize,
    void *context)
{
    (void)authorize;
    (void)context;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

/*
 * EN: Validates and sets `policy_diagnostic_authorizer` in Profile compatibility Stub state.
 * 中文：验证并设置 Profile 兼容 Stub 状态中的 `policy_diagnostic_authorizer`。
 */
ucn_result_t ucn_node_set_policy_diagnostic_authorizer(
    ucn_node_t *node,
    ucn_policy_diagnostic_authorize_fn authorize,
    void *context)
{
    (void)authorize;
    (void)context;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

/*
 * EN: Validates and sets `path_control_authorizer` in Profile compatibility Stub state.
 * 中文：验证并设置 Profile 兼容 Stub 状态中的 `path_control_authorizer`。
 */
ucn_result_t ucn_node_set_path_control_authorizer(
    ucn_node_t *node,
    ucn_path_control_authorize_fn authorize,
    void *context)
{
    (void)authorize;
    (void)context;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

/*
 * EN: Validates and sets `route_policy` in Profile compatibility Stub state.
 * 中文：验证并设置 Profile 兼容 Stub 状态中的 `route_policy`。
 */
ucn_result_t ucn_node_set_route_policy(ucn_node_t *node,
                                       const ucn_route_policy_config_t *config)
{
    (void)config;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

/*
 * EN: Clears `route_policy` from Profile compatibility Stub without allocating memory.
 * 中文：从 Profile 兼容 Stub 中清除 `route_policy`，且不进行动态分配。
 */
ucn_result_t ucn_node_clear_route_policy(ucn_node_t *node,
                                         const ucn_route_policy_key_t *key)
{
    (void)key;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

/*
 * EN: Searches bounded Profile compatibility Stub state for `route_policy`.
 * 中文：在固定容量的 Profile 兼容 Stub 状态中查找 `route_policy`。
 */
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

/*
 * EN: Validates and sets `policy_path` in Profile compatibility Stub state.
 * 中文：验证并设置 Profile 兼容 Stub 状态中的 `policy_path`。
 */
ucn_result_t ucn_node_set_policy_path(ucn_node_t *node,
                                      const ucn_policy_path_config_t *config)
{
    (void)config;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

/*
 * EN: Clears `policy_path` from Profile compatibility Stub without allocating memory.
 * 中文：从 Profile 兼容 Stub 中清除 `policy_path`，且不进行动态分配。
 */
ucn_result_t ucn_node_clear_policy_path(ucn_node_t *node,
                                        uint16_t local_path_id)
{
    (void)local_path_id;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

/*
 * EN: Searches bounded Profile compatibility Stub state for `policy_path`.
 * 中文：在固定容量的 Profile 兼容 Stub 状态中查找 `policy_path`。
 */
const ucn_policy_path_entry_t *ucn_node_find_policy_path(
    const ucn_node_t *node,
    uint16_t local_path_id)
{
    (void)node;
    (void)local_path_id;
    return NULL;
}

/*
 * EN: Validates and installs `bind_q1_flow` into bounded Profile compatibility Stub state.
 * 中文：验证 `bind_q1_flow` 并将其安装到固定容量的 Profile 兼容 Stub 状态中。
 */
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

/*
 * EN: Searches bounded Profile compatibility Stub state for `q1_flow`.
 * 中文：在固定容量的 Profile 兼容 Stub 状态中查找 `q1_flow`。
 */
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

/*
 * EN: Returns the current `link_quality` view from Profile compatibility Stub state.
 * 中文：从 Profile 兼容 Stub 状态返回当前 `link_quality` 视图。
 */
const ucn_policy_link_quality_snapshot_t *ucn_node_get_link_quality(
    const ucn_node_t *node,
    const ucn_link_t *link)
{
    (void)node;
    (void)link;
    return NULL;
}

/*
 * EN: Returns the current `policy_stats` view from Profile compatibility Stub state.
 * 中文：从 Profile 兼容 Stub 状态返回当前 `policy_stats` 视图。
 */
const ucn_policy_stats_t *ucn_node_get_policy_stats(const ucn_node_t *node)
{
    (void)node;
    return NULL;
}

/*
 * EN: Validates and installs `install_local_path` into bounded Profile compatibility Stub state.
 * 中文：验证 `install_local_path` 并将其安装到固定容量的 Profile 兼容 Stub 状态中。
 */
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

/*
 * EN: Validates and installs `install_local_path_capable` into bounded Profile compatibility Stub state.
 * 中文：验证 `install_local_path_capable` 并将其安装到固定容量的 Profile 兼容 Stub 状态中。
 */
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

/*
 * EN: Removes or releases `revoke_local_path` from Profile compatibility Stub state with bounded work.
 * 中文：以有界工作量从 Profile 兼容 Stub 状态移除或释放 `revoke_local_path`。
 */
ucn_result_t ucn_node_revoke_local_path(ucn_node_t *node,
                                        ucn_path_id_t path_id,
                                        ucn_node_id_t destination)
{
    (void)path_id;
    (void)destination;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

/*
 * EN: Validates and submits `send_path_install` through the bounded Profile compatibility Stub transmit path.
 * 中文：验证 `send_path_install` 并将其提交到有界的 Profile 兼容 Stub 发送路径。
 */
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

/*
 * EN: Validates and submits `send_path_install_capable` through the bounded Profile compatibility Stub transmit path.
 * 中文：验证 `send_path_install_capable` 并将其提交到有界的 Profile 兼容 Stub 发送路径。
 */
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

/*
 * EN: Validates and submits `send_path_revoke` through the bounded Profile compatibility Stub transmit path.
 * 中文：验证 `send_path_revoke` 并将其提交到有界的 Profile 兼容 Stub 发送路径。
 */
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

/*
 * EN: Searches bounded Profile compatibility Stub state for `path_forward`.
 * 中文：在固定容量的 Profile 兼容 Stub 状态中查找 `path_forward`。
 */
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

/*
 * EN: Validates and submits `send_path` through the bounded Profile compatibility Stub transmit path.
 * 中文：验证 `send_path` 并将其提交到有界的 Profile 兼容 Stub 发送路径。
 */
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

/*
 * EN: Builds and submits `request_path_trace` through the bounded Profile compatibility Stub transmit path.
 * 中文：构造 `request_path_trace` 并将其提交到有界的 Profile 兼容 Stub 发送路径。
 */
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

/*
 * EN: Builds and submits `request_node_snapshot` through the bounded Profile compatibility Stub transmit path.
 * 中文：构造 `request_node_snapshot` 并将其提交到有界的 Profile 兼容 Stub 发送路径。
 */
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

/*
 * EN: Builds and submits `request_policy_diagnostic` through the bounded Profile compatibility Stub transmit path.
 * 中文：构造 `request_policy_diagnostic` 并将其提交到有界的 Profile 兼容 Stub 发送路径。
 */
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
