#include <string.h>

#include "ucn/ucn.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node_storage.h"
#include "ucn/ucn_time.h"

#include "ucn_duplicate_internal.h"

/* Hardware-isolation seam used only when an embedding product explicitly
 * defines the macro. The default value preserves the complete production
 * Step pipeline. Values 1..6 stop after progressively later maintenance
 * phases so an MCU test can isolate one bounded Step phase without forking
 * the protocol implementation. */
#ifndef UCN_TEST_NODE_STEP_STAGE_LIMIT
#define UCN_TEST_NODE_STEP_STAGE_LIMIT 7
#endif
#if UCN_TEST_NODE_STEP_STAGE_LIMIT < 1 || UCN_TEST_NODE_STEP_STAGE_LIMIT > 7
#error "UCN_TEST_NODE_STEP_STAGE_LIMIT must be in [1, 7]"
#endif

/* Hardware-isolation seam for the Stage-7 maintenance scheduler. The
 * production default (5) executes every maintenance class. Test firmware may
 * stop after an earlier class to identify the first driver-facing operation
 * that destabilizes a target without changing the public UCN API or ABI. */
#ifndef UCN_TEST_NODE_MAINTENANCE_STAGE_LIMIT
#define UCN_TEST_NODE_MAINTENANCE_STAGE_LIMIT 5
#endif
#if UCN_TEST_NODE_MAINTENANCE_STAGE_LIMIT < 1 || \
    UCN_TEST_NODE_MAINTENANCE_STAGE_LIMIT > 5
#error "UCN_TEST_NODE_MAINTENANCE_STAGE_LIMIT must be in [1, 5]"
#endif

/* Full-profile hardware isolation for live Link metrics. The default keeps
 * Policy refresh enabled; an embedding test may disable only this phase while
 * retaining the Full object layout and the rest of the Step scheduler. */
#ifndef UCN_TEST_NODE_POLICY_REFRESH_ENABLED
#define UCN_TEST_NODE_POLICY_REFRESH_ENABLED 1
#endif
#if UCN_TEST_NODE_POLICY_REFRESH_ENABLED != 0 && \
    UCN_TEST_NODE_POLICY_REFRESH_ENABLED != 1
#error "UCN_TEST_NODE_POLICY_REFRESH_ENABLED must be 0 or 1"
#endif

/* Hardware-only heartbeat response isolation.  Production keeps immediate
 * one-hop ACKs enabled.  A target bench may disable only the response side so
 * scheduled Heartbeat request TX and full RX validation remain active while
 * proving whether a driver failure requires RX-pump -> Link TX nesting. */
#ifndef UCN_TEST_NODE_HEARTBEAT_ACK_ENABLED
#define UCN_TEST_NODE_HEARTBEAT_ACK_ENABLED 1
#endif
#if UCN_TEST_NODE_HEARTBEAT_ACK_ENABLED != 0 && \
    UCN_TEST_NODE_HEARTBEAT_ACK_ENABLED != 1
#error "UCN_TEST_NODE_HEARTBEAT_ACK_ENABLED must be 0 or 1"
#endif

static ucn_result_t get_link_status(const ucn_link_t *link,
                                    ucn_link_status_t *status);

#define UCN_ROUTE_REQUEST_ID_BYTES ((size_t)4U)
#define UCN_ROUTE_CONTROL_TRAILER_BYTES ((size_t)2U)
#define UCN_ROUTE_REQ_MAX_PAYLOAD_BYTES ((size_t)14U)
#define UCN_ROUTE_REPLY_MAX_PAYLOAD_BYTES ((size_t)12U)
#define UCN_ROUTE_ERROR_MAX_PAYLOAD_BYTES ((size_t)12U)
#define UCN_HELLO_PAYLOAD_BYTES ((size_t)1U)
#define UCN_HEARTBEAT_PAYLOAD_BYTES ((size_t)8U)
#define UCN_PATH_PROBE_PAYLOAD_BYTES ((size_t)12U)
#define UCN_PATH_ACTIVATE_PAYLOAD_BYTES ((size_t)6U)
#define UCN_PATH_INSTALL_MAX_PAYLOAD_BYTES ((size_t)20U)
#define UCN_PATH_REVOKE_MAX_PAYLOAD_BYTES ((size_t)8U)
#define UCN_PATH_TRACE_TRACE_ID_OFFSET ((size_t)0U)
#define UCN_PATH_TRACE_RECORD_COUNT_OFFSET ((size_t)4U)
#define UCN_PATH_TRACE_RECORD_LIMIT_OFFSET ((size_t)5U)
#define UCN_PATH_TRACE_STATUS_OFFSET ((size_t)6U)
#define UCN_PATH_TRACE_RESERVED_OFFSET ((size_t)7U)
#define UCN_PATH_TRACE_NODE_IDS_OFFSET UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES
#define UCN_PATH_TRACE_MAX_PAYLOAD_BYTES \
    (UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES + \
     UCN_PATH_TRACE_MAX_NODES * sizeof(ucn_node_id_t))
#define UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET ((size_t)0U)
#define UCN_NODE_SNAPSHOT_REQUEST_FLAGS_OFFSET ((size_t)4U)
#define UCN_NODE_SNAPSHOT_REPLY_MAX_PAYLOAD_BYTES ((size_t)12U)
#define UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET ((size_t)0U)
#define UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET ((size_t)4U)
#define UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET ((size_t)5U)
#define UCN_POLICY_DIAGNOSTIC_REQUEST_RESERVED_OFFSET ((size_t)6U)
#define UCN_POLICY_DIAGNOSTIC_REPLY_STATUS_OFFSET ((size_t)6U)
#define UCN_POLICY_DIAGNOSTIC_REPLY_RESERVED_OFFSET ((size_t)7U)
#define UCN_POLICY_DIAGNOSTIC_RECORD_OFFSET ((size_t)8U)
#define UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_UP ((uint8_t)0x01U)
#define UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_ROUTE_COST ((uint8_t)0x02U)
#define UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_RTT ((uint8_t)0x04U)
#define UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_TX_FAILURE ((uint8_t)0x08U)
#define UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_QUEUE_PRESSURE ((uint8_t)0x10U)
#define UCN_HEARTBEAT_REQUEST ((uint8_t)1U)
#define UCN_HEARTBEAT_ACK ((uint8_t)2U)
#define UCN_ROUTE_REQ_FLAG_CANDIDATE ((uint8_t)0x01U)

static void invalidate_routes_by_link(ucn_node_t *node, const ucn_link_t *link);
static ucn_result_t get_link_status(const ucn_link_t *link, ucn_link_status_t *status);
static bool link_is_usable(const ucn_link_t *link);
static uint32_t link_heartbeat_interval_ms(const ucn_link_t *link);
static uint32_t initial_heartbeat_phase_ms(
    const ucn_node_t *node,
    const ucn_neighbor_entry_t *entry,
    const ucn_neighbor_bearer_t *bearer);
static uint32_t link_suspect_timeout_ms(const ucn_link_t *link);
static uint32_t link_remove_timeout_ms(const ucn_link_t *link);
static ucn_link_t *resolve_egress_link(ucn_node_t *node, ucn_link_t *link);
#if UCN_FEATURE_PATH
static void revoke_path_and_mark_local_policy(ucn_node_t *node,
                                               ucn_node_id_t owner,
                                               ucn_session_id_t owner_session_id,
                                               ucn_path_id_t path_id,
                                               ucn_node_id_t destination);
static void revoke_paths_by_unavailable_egress(ucn_node_t *node,
                                                ucn_link_t *failed_link);
#endif
static ucn_result_t begin_route_discovery(ucn_node_t *node,
                                          ucn_node_id_t destination,
                                          uint32_t now_ms,
                                          bool is_candidate,
                                          uint8_t maximum_hop_limit,
                                          bool restart_active,
                                          bool require_verified_rtt);
static ucn_result_t send_endpoint_internal(
    ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class,
    const uint8_t *payload,
    uint16_t payload_length,
    bool allow_pending_queue);
#if UCN_FEATURE_CANDIDATE_ROUTING
static void expire_candidate_routes(ucn_node_t *node);
#endif
#if UCN_FEATURE_PATH
static ucn_result_t send_path_route_error(ucn_node_t *node,
                                          ucn_link_t *upstream_link,
                                          ucn_node_id_t origin,
                                          ucn_node_id_t unreachable,
                                          ucn_session_id_t owner_session_id,
                                          ucn_path_id_t path_id,
                                          ucn_wire_profile_t wire_profile);
#endif

/*
 * EN: Reads `u32_be` from the canonical Lite/Full Node byte order.
 * 中文：按规范的 Lite/Full Node 字节序读取 `u32_be`。
 */
static uint32_t read_u32_be(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) | (uint32_t)data[3];
}

/*
 * EN: Writes `u32_be` in the canonical Lite/Full Node byte order.
 * 中文：按规范的 Lite/Full Node 字节序写入 `u32_be`。
 */
static void write_u32_be(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24U);
    data[1] = (uint8_t)(value >> 16U);
    data[2] = (uint8_t)(value >> 8U);
    data[3] = (uint8_t)value;
}

#if UCN_FEATURE_CANDIDATE_ROUTING || UCN_FEATURE_DIAGNOSTICS
/*
 * EN: Reads `u16_be` from the canonical Lite/Full Node byte order.
 * 中文：按规范的 Lite/Full Node 字节序读取 `u16_be`。
 */
static uint16_t read_u16_be(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | (uint16_t)data[1]);
}

/*
 * EN: Writes `u16_be` in the canonical Lite/Full Node byte order.
 * 中文：按规范的 Lite/Full Node 字节序写入 `u16_be`。
 */
static void write_u16_be(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8U);
    data[1] = (uint8_t)value;
}
#endif

/*
 * EN: Reads `uint_be` from the canonical Lite/Full Node byte order.
 * 中文：按规范的 Lite/Full Node 字节序读取 `uint_be`。
 */
static uint32_t read_uint_be(const uint8_t *data, uint8_t width)
{
    uint8_t index;
    uint32_t value = 0U;

    for (index = 0U; index < width; ++index) {
        value = (value << 8U) | data[index];
    }
    return value;
}

/*
 * EN: Writes `uint_be` in the canonical Lite/Full Node byte order.
 * 中文：按规范的 Lite/Full Node 字节序写入 `uint_be`。
 */
static void write_uint_be(uint8_t *data, uint8_t width, uint32_t value)
{
    uint8_t index;

    for (index = 0U; index < width; ++index) {
        const uint8_t shift = (uint8_t)((width - index - 1U) * 8U);

        data[index] = (uint8_t)(value >> shift);
    }
}

/*
 * EN: Calculates the bounded `route_request_payload_size` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `route_request_payload_size` 值。
 */
static size_t route_request_payload_size(ucn_wire_profile_t profile)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);

    return descriptor == NULL ? 0U :
           (size_t)descriptor->address_bytes + UCN_ROUTE_REQUEST_ID_BYTES +
               (size_t)descriptor->route_cost_bytes +
               UCN_ROUTE_CONTROL_TRAILER_BYTES;
}

/*
 * EN: Calculates the bounded `route_request_id_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `route_request_id_offset` 值。
 */
static size_t route_request_id_offset(const ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(frame->wire_profile);

    return descriptor == NULL ? 0U : descriptor->address_bytes;
}

/*
 * EN: Calculates the bounded `route_request_cost_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `route_request_cost_offset` 值。
 */
static size_t route_request_cost_offset(const ucn_frame_t *frame)
{
    return route_request_id_offset(frame) + UCN_ROUTE_REQUEST_ID_BYTES;
}

/*
 * EN: Calculates the bounded `route_request_hop_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `route_request_hop_offset` 值。
 */
static size_t route_request_hop_offset(const ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(frame->wire_profile);

    return descriptor == NULL ? 0U :
           route_request_cost_offset(frame) + descriptor->route_cost_bytes;
}

/*
 * EN: Calculates the bounded `route_request_flags_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `route_request_flags_offset` 值。
 */
static size_t route_request_flags_offset(const ucn_frame_t *frame)
{
    return route_request_hop_offset(frame) + 1U;
}

/*
 * EN: Builds and submits `route_request_target` through the bounded Lite/Full Node transmit path.
 * 中文：构造 `route_request_target` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_node_id_t route_request_target(const ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(frame->wire_profile);

    return descriptor == NULL ? 0U :
           read_uint_be(frame->payload, descriptor->address_bytes);
}

/*
 * EN: Calculates the bounded `route_reply_payload_size` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `route_reply_payload_size` 值。
 */
static size_t route_reply_payload_size(ucn_wire_profile_t profile)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);

    return descriptor == NULL ? 0U :
           UCN_ROUTE_REQUEST_ID_BYTES + descriptor->route_cost_bytes +
               UCN_ROUTE_CONTROL_TRAILER_BYTES +
               descriptor->route_epoch_bytes;
}

/*
 * EN: Calculates the bounded `route_reply_cost_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `route_reply_cost_offset` 值。
 */
static size_t route_reply_cost_offset(void)
{
    return UCN_ROUTE_REQUEST_ID_BYTES;
}

/*
 * EN: Calculates the bounded `route_reply_hop_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `route_reply_hop_offset` 值。
 */
static size_t route_reply_hop_offset(const ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(frame->wire_profile);

    return descriptor == NULL ? 0U :
           route_reply_cost_offset() + descriptor->route_cost_bytes;
}

/*
 * EN: Calculates the bounded `route_reply_flags_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `route_reply_flags_offset` 值。
 */
static size_t route_reply_flags_offset(const ucn_frame_t *frame)
{
    return route_reply_hop_offset(frame) + 1U;
}

/*
 * EN: Calculates the bounded `route_reply_epoch_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `route_reply_epoch_offset` 值。
 */
static size_t route_reply_epoch_offset(const ucn_frame_t *frame)
{
    return route_reply_flags_offset(frame) + 1U;
}

/*
 * EN: Calculates the bounded `route_error_payload_size` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `route_error_payload_size` 值。
 */
static size_t route_error_payload_size(ucn_wire_profile_t profile,
                                       bool path_scoped)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);

    if (descriptor == NULL) {
        return 0U;
    }
    return path_scoped ?
               (size_t)descriptor->address_bytes +
                   (size_t)descriptor->address_bytes +
                   (size_t)descriptor->path_id_bytes :
               (size_t)descriptor->address_bytes;
}

#if UCN_FEATURE_PATH
/*
 * EN: Calculates the bounded `path_install_base_payload_size` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `path_install_base_payload_size` 值。
 */
static size_t path_install_base_payload_size(ucn_wire_profile_t profile)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);

    return descriptor == NULL ? 0U :
           (size_t)descriptor->path_id_bytes +
               (size_t)descriptor->address_bytes * 2U + 5U;
}

/*
 * EN: Calculates the bounded `path_install_capable_payload_size` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `path_install_capable_payload_size` 值。
 */
static size_t path_install_capable_payload_size(ucn_wire_profile_t profile)
{
    const size_t base_size = path_install_base_payload_size(profile);

    return base_size == 0U ? 0U : base_size + 3U;
}

/*
 * EN: Checks the `path_install_payload_length_supported` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `path_install_payload_length_supported` 条件。
 */
static bool path_install_payload_length_supported(
    ucn_wire_profile_t profile,
    uint16_t payload_length)
{
    const size_t base_size = path_install_base_payload_size(profile);
    const size_t capable_size = path_install_capable_payload_size(profile);

    return base_size != 0U &&
           ((size_t)payload_length == base_size ||
            (size_t)payload_length == capable_size);
}

/*
 * EN: Calculates the bounded `path_install_destination_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `path_install_destination_offset` 值。
 */
static size_t path_install_destination_offset(
    const ucn_wire_profile_descriptor_t *descriptor)
{
    return descriptor == NULL ? 0U : descriptor->path_id_bytes;
}

/*
 * EN: Calculates the bounded `path_install_next_hop_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `path_install_next_hop_offset` 值。
 */
static size_t path_install_next_hop_offset(
    const ucn_wire_profile_descriptor_t *descriptor)
{
    return descriptor == NULL ? 0U :
           (size_t)descriptor->path_id_bytes + descriptor->address_bytes;
}

/*
 * EN: Calculates the bounded `path_install_lease_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `path_install_lease_offset` 值。
 */
static size_t path_install_lease_offset(
    const ucn_wire_profile_descriptor_t *descriptor)
{
    return descriptor == NULL ? 0U :
           (size_t)descriptor->path_id_bytes +
               (size_t)descriptor->address_bytes * 2U;
}

/*
 * EN: Calculates the bounded `path_install_remaining_hops_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `path_install_remaining_hops_offset` 值。
 */
static size_t path_install_remaining_hops_offset(
    const ucn_wire_profile_descriptor_t *descriptor)
{
    return path_install_lease_offset(descriptor) + 4U;
}

/*
 * EN: Calculates the bounded `path_install_maximum_profile_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `path_install_maximum_profile_offset` 值。
 */
static size_t path_install_maximum_profile_offset(
    const ucn_wire_profile_descriptor_t *descriptor)
{
    return path_install_remaining_hops_offset(descriptor) + 1U;
}

/*
 * EN: Calculates the bounded `path_install_minimum_mtu_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `path_install_minimum_mtu_offset` 值。
 */
static size_t path_install_minimum_mtu_offset(
    const ucn_wire_profile_descriptor_t *descriptor)
{
    return path_install_maximum_profile_offset(descriptor) + 1U;
}

/*
 * EN: Calculates the bounded `path_revoke_payload_size` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `path_revoke_payload_size` 值。
 */
static size_t path_revoke_payload_size(ucn_wire_profile_t profile)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);

    return descriptor == NULL ? 0U :
           (size_t)descriptor->path_id_bytes + descriptor->address_bytes;
}
#endif

#if UCN_FEATURE_DIAGNOSTICS
/*
 * EN: Calculates the bounded `path_trace_payload_size` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `path_trace_payload_size` 值。
 */
static size_t path_trace_payload_size(ucn_wire_profile_t profile,
                                      uint8_t record_count)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);

    return descriptor == NULL ? 0U :
           UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES +
               (size_t)record_count * descriptor->address_bytes;
}

/*
 * EN: Calculates the bounded `node_snapshot_reply_payload_size` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `node_snapshot_reply_payload_size` 值。
 */
static size_t node_snapshot_reply_payload_size(ucn_wire_profile_t profile)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);

    return descriptor == NULL ? 0U :
           4U + (size_t)descriptor->address_bytes + 4U;
}

/*
 * EN: Calculates the bounded `node_snapshot_reply_neighbor_count_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `node_snapshot_reply_neighbor_count_offset` 值。
 */
static size_t node_snapshot_reply_neighbor_count_offset(
    const ucn_wire_profile_descriptor_t *descriptor)
{
    return descriptor == NULL ? 0U : 4U + descriptor->address_bytes;
}

/*
 * EN: Calculates the bounded `node_snapshot_reply_flags_offset` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `node_snapshot_reply_flags_offset` 值。
 */
static size_t node_snapshot_reply_flags_offset(
    const ucn_wire_profile_descriptor_t *descriptor)
{
    return node_snapshot_reply_neighbor_count_offset(descriptor) + 1U;
}
#endif

/*
 * EN: Selects or resolves `select_route_request_profile` using deterministic Lite/Full Node rules.
 * 中文：按照确定性的 Lite/Full Node 规则选择或解析 `select_route_request_profile`。
 */
static ucn_result_t select_route_request_profile(
    const ucn_node_t *node,
    ucn_node_id_t destination,
    uint8_t hop_limit,
    ucn_wire_profile_t *selected_profile)
{
    ucn_wire_profile_t profile;

    if (node == NULL || selected_profile == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!node->automatic_wire_profile) {
        *selected_profile = node->tx_wire_profile;
        return destination <= ucn_wire_profile_get_descriptor(
                                  node->tx_wire_profile)->max_node_id ?
               UCN_OK : UCN_ERR_TOO_LARGE;
    }

    /* Route discovery has no single peer ceiling: it is flooded over every
     * eligible Link.  Choose the smallest profile that represents the whole
     * configured search domain; each egress Link then applies MTU and learned
     * peer-RX checks independently without widening the RREQ in transit. */
    for (profile = UCN_WIRE_PROFILE_W0_LOCAL;
         profile <= node->tx_wire_profile; ++profile) {
        const ucn_wire_profile_descriptor_t *descriptor =
            ucn_wire_profile_get_descriptor(profile);

        if (descriptor != NULL &&
            node->config.network_id <= descriptor->max_wire_value &&
            node->config.node_id <= descriptor->max_node_id &&
            destination <= descriptor->max_node_id &&
            hop_limit <= descriptor->max_hops &&
            node->session_id <= descriptor->max_wire_value) {
            *selected_profile = profile;
            return UCN_OK;
        }
    }
    return UCN_ERR_TOO_LARGE;
}

/*
 * EN: Calculates `maximum_value_for_wire_bytes` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `maximum_value_for_wire_bytes`。
 */
static uint32_t maximum_value_for_wire_bytes(uint8_t width)
{
    return width >= 4U ? UINT32_MAX :
           (UINT32_C(1) << ((uint32_t)width * 8U)) - UINT32_C(1);
}

/*
 * EN: Writes `route_cost_for_profile` in the canonical Lite/Full Node byte order.
 * 中文：按规范的 Lite/Full Node 字节序写入 `route_cost_for_profile`。
 */
static ucn_result_t write_route_cost_for_profile(
    uint8_t *output,
    ucn_wire_profile_t profile,
    ucn_route_cost_t route_cost)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);
    uint32_t wire_maximum;

    if (output == NULL || descriptor == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    wire_maximum = maximum_value_for_wire_bytes(descriptor->route_cost_bytes);
    if (route_cost == UCN_ROUTE_COST_UNKNOWN) {
        write_uint_be(output, descriptor->route_cost_bytes, wire_maximum);
        return UCN_OK;
    }
    if (route_cost >= wire_maximum) {
        return UCN_ERR_TOO_LARGE;
    }
    write_uint_be(output, descriptor->route_cost_bytes, route_cost);
    return UCN_OK;
}

/*
 * EN: Reads `route_cost_for_profile` from the canonical Lite/Full Node byte order.
 * 中文：按规范的 Lite/Full Node 字节序读取 `route_cost_for_profile`。
 */
static ucn_route_cost_t read_route_cost_for_profile(
    const uint8_t *input,
    ucn_wire_profile_t profile)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);
    uint32_t wire_value;
    uint32_t wire_maximum;

    if (input == NULL || descriptor == NULL) {
        return UCN_ROUTE_COST_UNKNOWN;
    }
    wire_value = read_uint_be(input, descriptor->route_cost_bytes);
    wire_maximum = maximum_value_for_wire_bytes(descriptor->route_cost_bytes);
    return wire_value == wire_maximum ? UCN_ROUTE_COST_UNKNOWN : wire_value;
}

#if UCN_FEATURE_PATH
/*
 * EN: Validates `path_wire_scope` before Lite/Full Node state is used or changed.
 * 中文：在使用或修改 Lite/Full Node 状态前验证 `path_wire_scope`。
 */
static ucn_result_t validate_path_wire_scope(ucn_wire_profile_t profile,
                                             ucn_path_id_t path_id,
                                             ucn_node_id_t destination,
                                             ucn_node_id_t next_hop)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);

    if (descriptor == NULL) {
        return UCN_ERR_CONFIG;
    }
    if (path_id > maximum_value_for_wire_bytes(descriptor->path_id_bytes) ||
        destination > descriptor->max_node_id ||
        (next_hop != 0U && next_hop > descriptor->max_node_id)) {
        return UCN_ERR_TOO_LARGE;
    }
    return UCN_OK;
}
#endif

/*
 * EN: Calculates `accumulate_route_cost` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `accumulate_route_cost`。
 */
static ucn_result_t accumulate_route_cost(ucn_route_cost_t left,
                                          ucn_route_cost_t right,
                                          ucn_route_cost_t *output)
{
    if (output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (left == UCN_ROUTE_COST_UNKNOWN || right == UCN_ROUTE_COST_UNKNOWN) {
        *output = UCN_ROUTE_COST_UNKNOWN;
        return UCN_OK;
    }
    if (left > UCN_ROUTE_COST_MAX - right) {
        return UCN_ERR_TOO_LARGE;
    }
    *output = left + right;
    return UCN_OK;
}

/*
 * EN: Checks the current `route_cost_is_known` condition in Lite/Full Node state.
 * 中文：检查当前 Lite/Full Node 状态中的 `route_cost_is_known` 条件。
 */
static bool route_cost_is_known(ucn_route_cost_t route_cost)
{
    return route_cost != 0U && route_cost != UCN_ROUTE_COST_UNKNOWN;
}

/*
 * EN: Checks the current `route_cost_is_better` condition in Lite/Full Node state.
 * 中文：检查当前 Lite/Full Node 状态中的 `route_cost_is_better` 条件。
 */
static bool route_cost_is_better(ucn_route_cost_t candidate_cost,
                                 ucn_route_cost_t active_cost)
{
    if (!route_cost_is_known(candidate_cost)) {
        return false;
    }
    return !route_cost_is_known(active_cost) || candidate_cost < active_cost;
}

/*
 * EN: Derives `route_epoch_from_request_id` with the canonical Lite/Full Node conversion rules.
 * 中文：按照规范的 Lite/Full Node 转换规则推导 `route_epoch_from_request_id`。
 */
static uint16_t route_epoch_from_request_id(ucn_wire_profile_t profile,
                                            uint32_t request_id)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);
    uint16_t route_epoch = (uint16_t)(request_id ^ (request_id >> 16U));

    if (descriptor != NULL && descriptor->route_epoch_bytes == 1U) {
        route_epoch = (uint16_t)(route_epoch & UINT16_C(0x00FF));
    }
    return route_epoch == 0U ? 1U : route_epoch;
}

/*
 * EN: Calculates `link_route_cost` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `link_route_cost`。
 */
static ucn_route_cost_t link_route_cost(const ucn_link_t *link)
{
    ucn_link_metrics_t metrics;

    (void)memset(&metrics, 0, sizeof(metrics));
    if (link != NULL && link->ops != NULL && link->ops->get_metrics != NULL &&
        link->ops->get_metrics(link, &metrics) == UCN_OK &&
        metrics.route_cost_valid && metrics.route_cost != 0U &&
        metrics.route_cost != UCN_LINK_ROUTE_COST_UNKNOWN) {
        return (ucn_route_cost_t)metrics.route_cost;
    }
    return UCN_ROUTE_COST_UNKNOWN;
}

/* LC-1 never replaces the additive on-wire Cost.  This helper exposes the
 * Full-only local selection score and deliberately falls back to the stable
 * base Cost in Lite builds or before the first quality sample. */
/*
 * EN: Calculates `link_local_select_cost` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `link_local_select_cost`。
 */
static bool link_local_select_cost(ucn_node_t *node,
                                   ucn_link_t *link,
                                   bool *cost_known,
                                   uint16_t *cost)
{
    ucn_route_cost_t base_cost;

    if (node == NULL || link == NULL || cost_known == NULL || cost == NULL ||
        !link_is_usable(link)) {
        return false;
    }
#if UCN_FEATURE_POLICY
    {
        const ucn_policy_link_quality_snapshot_t *quality =
            ucn_node_get_link_quality(node, link);

        if (quality != NULL) {
            if (!quality->cost.selectable) {
                return false;
            }
            *cost_known = quality->cost.base_cost_known;
            *cost = quality->cost.effective_select_cost;
            return true;
        }
    }
#endif
    base_cost = link_route_cost(link);
    *cost_known = route_cost_is_known(base_cost);
    *cost = *cost_known ? (uint16_t)base_cost : UCN_LINK_ROUTE_COST_UNKNOWN;
    return true;
}

/*
 * EN: Checks or removes expired `route_is_expired` state in Lite/Full Node.
 * 中文：检查或移除 Lite/Full Node 中已过期的 `route_is_expired` 状态。
 */
static bool route_is_expired(const ucn_node_t *node, const ucn_route_entry_t *route)
{
    return !route->is_static &&
           ucn_deadline_expired(node->now_ms, route->expires_at_ms);
}

#if UCN_FEATURE_CANDIDATE_ROUTING
/*
 * EN: Checks or removes expired `candidate_is_expired` state in Lite/Full Node.
 * 中文：检查或移除 Lite/Full Node 中已过期的 `candidate_is_expired` 状态。
 */
static bool candidate_is_expired(const ucn_node_t *node,
                                 const ucn_candidate_route_t *candidate)
{
    return ucn_deadline_expired(node->now_ms, candidate->expires_at_ms);
}
#endif

/*
 * EN: Calculates `cost_is_sufficiently_better` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `cost_is_sufficiently_better`。
 */
static bool cost_is_sufficiently_better(ucn_route_cost_t active_cost,
                                        ucn_route_cost_t candidate_cost,
                                        uint8_t improvement_percent)
{
    uint64_t candidate_scaled;
    uint64_t active_scaled;

    if (!route_cost_is_known(candidate_cost)) {
        return false;
    }
    if (!route_cost_is_known(active_cost)) {
        return true;
    }
    if (candidate_cost >= active_cost) {
        return false;
    }
    candidate_scaled = (uint64_t)candidate_cost * UINT64_C(100);
    active_scaled = (uint64_t)active_cost *
                     (uint32_t)(100U - improvement_percent);
    return candidate_scaled <= active_scaled;
}

#if UCN_FEATURE_CANDIDATE_ROUTING
/*
 * EN: Checks the current `candidate_is_sufficiently_better` condition in Lite/Full Node state.
 * 中文：检查当前 Lite/Full Node 状态中的 `candidate_is_sufficiently_better` 条件。
 */
static bool candidate_is_sufficiently_better(ucn_route_cost_t active_cost,
                                             ucn_route_cost_t candidate_cost)
{
    return cost_is_sufficiently_better(active_cost, candidate_cost,
                                       UCN_ROUTE_SWITCH_IMPROVEMENT_PERCENT);
}

/* Replace only this node's stable one-hop base contribution with LC-1's
 * local effective score.  The stored/forwarded multi-hop route_cost remains
 * byte-for-byte unchanged. */
/*
 * EN: Calculates `route_local_selection_score` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `route_local_selection_score`。
 */
static bool route_local_selection_score(ucn_node_t *node,
                                        ucn_route_cost_t route_cost,
                                        ucn_link_t *local_link,
                                        bool *score_known,
                                        ucn_route_cost_t *score)
{
    ucn_route_cost_t local_base;
    uint16_t local_effective;
    bool local_effective_known;
    uint64_t adjusted;

    if (score_known == NULL || score == NULL ||
        !link_local_select_cost(node, local_link, &local_effective_known,
                                &local_effective)) {
        return false;
    }
    *score_known = route_cost_is_known(route_cost);
    *score = route_cost;
    if (!*score_known) {
        return true;
    }
    local_base = link_route_cost(local_link);
    if (!local_effective_known || !route_cost_is_known(local_base) ||
        route_cost < local_base) {
        return true;
    }
    adjusted = (uint64_t)(route_cost - local_base) + local_effective;
    *score = adjusted > UCN_ROUTE_COST_MAX ? UCN_ROUTE_COST_MAX :
                                              (ucn_route_cost_t)adjusted;
    return true;
}

/*
 * EN: Checks the current `candidate_route_is_locally_better` condition in Lite/Full Node state.
 * 中文：检查当前 Lite/Full Node 状态中的 `candidate_route_is_locally_better` 条件。
 */
static bool candidate_route_is_locally_better(
    ucn_node_t *node,
    const ucn_route_entry_t *active_route,
    ucn_route_cost_t candidate_route_cost,
    ucn_link_t *candidate_link)
{
    ucn_route_cost_t active_score;
    ucn_route_cost_t candidate_score;
    ucn_link_t *active_link;
    bool active_known;
    bool candidate_known;

    if (!route_local_selection_score(node, candidate_route_cost, candidate_link,
                                     &candidate_known, &candidate_score)) {
        return false;
    }
    if (active_route == NULL) {
        return true;
    }
    active_link = resolve_egress_link(node, active_route->egress_link);
    if (!route_local_selection_score(node, active_route->route_cost, active_link,
                                     &active_known, &active_score)) {
        return true;
    }
    if (!candidate_known) {
        return false;
    }
    if (!active_known) {
        return true;
    }
    return candidate_is_sufficiently_better(active_score, candidate_score);
}
#endif

/*
 * EN: Searches bounded Lite/Full Node state for `direct_link`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `direct_link`。
 */
static ucn_link_t *find_direct_link(ucn_node_t *node,
                                    ucn_node_id_t destination)
{
    size_t index;
    ucn_link_t *best_direct = NULL;
    ucn_link_t *fallback_direct = NULL;
    uint16_t best_direct_cost = UCN_LINK_ROUTE_COST_UNKNOWN;
    bool best_cost_known = false;

    for (index = 0U; index < node->link_count; ++index) {
        ucn_link_t *link = node->links[index];
        ucn_link_t *selected;
        uint16_t select_cost;
        bool cost_known;

        if (link->peer_node_id != destination) {
            continue;
        }
        if (!link_is_usable(link) &&
            (fallback_direct == NULL ||
             link->link_id < fallback_direct->link_id)) {
            fallback_direct = link;
        }
        selected = resolve_egress_link(node, link);
        if (selected != link) {
            continue;
        }
        if (!link_local_select_cost(node, link, &cost_known, &select_cost)) {
            continue;
        }
        if (best_direct == NULL ||
            (cost_known && (!best_cost_known || select_cost < best_direct_cost)) ||
            (cost_known == best_cost_known && select_cost == best_direct_cost &&
             link->link_id < best_direct->link_id)) {
            best_direct = link;
            best_direct_cost = select_cost;
            best_cost_known = cost_known;
        }
    }

    /* Preserve the public send() error contract: if a direct Link exists but
     * is currently down/MTU-less, let send_frame_on_link() report the precise
     * LINK_DOWN/TOO_LARGE result instead of collapsing it into NOT_FOUND. */
    return best_direct == NULL ? fallback_direct : best_direct;
}

/*
 * EN: Searches bounded Lite/Full Node state for `link`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `link`。
 */
static ucn_link_t *find_link(ucn_node_t *node, ucn_node_id_t destination)
{
    size_t index;
    ucn_link_t *direct = find_direct_link(node, destination);

    if (direct != NULL) {
        return direct;
    }

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid &&
            !route_is_expired(node, &node->routes[index]) &&
            node->routes[index].destination == destination) {
            return resolve_egress_link(node, node->routes[index].egress_link);
        }
    }

    return NULL;
}

/*
 * EN: Searches bounded Lite/Full Node state for `link_for_route_epoch`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `link_for_route_epoch`。
 */
static ucn_link_t *find_link_for_route_epoch(ucn_node_t *node,
                                             ucn_node_id_t destination,
                                             bool has_route_extension,
                                             uint16_t route_epoch)
{
    size_t index;
    ucn_link_t *direct = find_direct_link(node, destination);

    if (direct != NULL) {
        return direct;
    }
    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        const ucn_route_entry_t *route = &node->routes[index];

        if (!route->valid || route_is_expired(node, route) ||
            route->destination != destination) {
            continue;
        }
        if (has_route_extension) {
            if (route->route_epoch == route_epoch) {
                return resolve_egress_link(node, route->egress_link);
            }
            if (route->previous_valid &&
                !ucn_deadline_expired(node->now_ms, route->previous_expires_at_ms) &&
                route->previous_route_epoch == route_epoch) {
                return resolve_egress_link(node, route->previous_egress_link);
            }
        } else if (route->route_epoch == 0U) {
            return resolve_egress_link(node, route->egress_link);
        } else if (route->previous_valid && route->previous_route_epoch == 0U &&
                   !ucn_deadline_expired(node->now_ms, route->previous_expires_at_ms)) {
            return resolve_egress_link(node, route->previous_egress_link);
        }
    }
    return NULL;
}

/*
 * EN: Checks the `route_epoch_is_accepted` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `route_epoch_is_accepted` 条件。
 */
static bool route_epoch_is_accepted(ucn_node_t *node,
                                    ucn_node_id_t source,
                                    const ucn_frame_t *frame)
{
    if (find_direct_link(node, source) != NULL) {
        return true;
    }
    return find_link_for_route_epoch(node, source, frame->has_route_extension,
                                     frame->route_epoch) != NULL;
}

/*
 * EN: Checks the current `link_is_registered` condition in Lite/Full Node state.
 * 中文：检查当前 Lite/Full Node 状态中的 `link_is_registered` 条件。
 */
static bool link_is_registered(const ucn_node_t *node, const ucn_link_t *link)
{
    size_t index;

    for (index = 0U; index < node->link_count; ++index) {
        if (node->links[index] == link) {
            return true;
        }
    }

    return false;
}

#if UCN_FEATURE_PATH
/*
 * EN: Searches bounded Lite/Full Node state for `active_path`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `active_path`。
 */
static const ucn_path_forward_entry_t *find_active_path(
    const ucn_node_t *node,
    ucn_node_id_t owner,
    ucn_session_id_t owner_session_id,
    ucn_path_id_t path_id,
    ucn_node_id_t destination)
{
    const ucn_path_forward_entry_t *entry = ucn_path_find(
        &node->path_state, owner, owner_session_id, path_id, destination);

    return ucn_path_is_expired(entry, node->now_ms) ? NULL : entry;
}
#endif

/*
 * EN: Searches bounded Lite/Full Node state for `neighbor`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `neighbor`。
 */
static ucn_neighbor_entry_t *find_neighbor(ucn_node_t *node,
                                            ucn_node_id_t peer_node_id)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        if (node->neighbors[index].state != UCN_NEIGHBOR_EMPTY &&
            node->neighbors[index].peer_node_id == peer_node_id) {
            return &node->neighbors[index];
        }
    }
    return NULL;
}

/*
 * EN: Searches bounded Lite/Full Node state for `neighbor_by_link`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `neighbor_by_link`。
 */
static ucn_neighbor_entry_t *find_neighbor_by_link(ucn_node_t *node,
                                                    const ucn_link_t *link)
{
    size_t index;
    size_t bearer_index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        if (node->neighbors[index].state == UCN_NEIGHBOR_EMPTY) {
            continue;
        }
        for (bearer_index = 0U;
             bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR;
             ++bearer_index) {
            if (node->neighbors[index].bearers[bearer_index].link == link) {
                return &node->neighbors[index];
            }
        }
    }
    return NULL;
}

/*
 * EN: Searches bounded Lite/Full Node state for `neighbor_bearer`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `neighbor_bearer`。
 */
static ucn_neighbor_bearer_t *find_neighbor_bearer(ucn_neighbor_entry_t *entry,
                                                    const ucn_link_t *link)
{
    size_t index;

    if (entry == NULL || link == NULL) {
        return NULL;
    }
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        if (entry->bearers[index].link == link) {
            return &entry->bearers[index];
        }
    }
    return NULL;
}

/*
 * EN: Allocates `allocate_neighbor_bearer` from fixed Lite/Full Node slots without heap use.
 * 中文：从 Lite/Full Node 的固定槽位分配 `allocate_neighbor_bearer`，不使用堆内存。
 */
static ucn_neighbor_bearer_t *allocate_neighbor_bearer(ucn_neighbor_entry_t *entry)
{
    size_t index;

    if (entry == NULL || entry->bearer_count >= UCN_MAX_BEARERS_PER_NEIGHBOR) {
        return NULL;
    }
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        if (entry->bearers[index].state == UCN_NEIGHBOR_BEARER_EMPTY) {
            return &entry->bearers[index];
        }
    }
    return NULL;
}

/*
 * EN: Calculates `adjust_route_cost_for_bearer_switch` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `adjust_route_cost_for_bearer_switch`。
 */
static void adjust_route_cost_for_bearer_switch(
    ucn_route_cost_t *route_cost,
    const ucn_link_t *old_link,
    const ucn_link_t *new_link)
{
    const ucn_route_cost_t old_local_cost = link_route_cost(old_link);
    const ucn_route_cost_t new_local_cost = link_route_cost(new_link);
    ucn_route_cost_t adjusted_cost;

    if (route_cost == NULL || old_link == new_link) {
        return;
    }
    if (!route_cost_is_known(*route_cost) ||
        !route_cost_is_known(old_local_cost) ||
        !route_cost_is_known(new_local_cost) ||
        *route_cost < old_local_cost ||
        accumulate_route_cost(*route_cost - old_local_cost, new_local_cost,
                              &adjusted_cost) != UCN_OK) {
        *route_cost = UCN_ROUTE_COST_UNKNOWN;
        return;
    }
    *route_cost = adjusted_cost;
}

/* Route/Previous/Candidate entries describe a logical next hop, not one
 * permanently pinned physical carrier.  Keep their stored representative in
 * sync with the Neighbor Primary so diagnostics, Route Epoch emission and
 * local Cost constraints cannot continue to reference an unadmitted Bearer. */
/*
 * EN: Remaps routes and Path references after a neighbor changes its primary Bearer.
 * 中文：在邻居切换主 Bearer 后重映射 Route 与 Path 引用。
 */
static void remap_neighbor_egress_references(ucn_node_t *node,
                                             ucn_neighbor_entry_t *entry,
                                             ucn_link_t *new_link)
{
    size_t index;

    if (node == NULL || entry == NULL || new_link == NULL) {
        return;
    }
    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        ucn_route_entry_t *route = &node->routes[index];

        if (route->valid && route->egress_link != new_link &&
            find_neighbor_bearer(entry, route->egress_link) != NULL) {
            adjust_route_cost_for_bearer_switch(&route->route_cost,
                                                route->egress_link, new_link);
            route->egress_link = new_link;
            route->verified_rtt_valid = false;
            route->verified_rtt_ms = 0U;
        }
        if (route->previous_valid && route->previous_egress_link != new_link &&
            find_neighbor_bearer(entry, route->previous_egress_link) != NULL) {
            route->previous_egress_link = new_link;
        }
    }
#if UCN_FEATURE_CANDIDATE_ROUTING
    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        ucn_candidate_route_t *candidate = &node->candidates[index];

        if (candidate->valid && candidate->egress_link != new_link &&
            find_neighbor_bearer(entry, candidate->egress_link) != NULL) {
            adjust_route_cost_for_bearer_switch(&candidate->route_cost,
                                                candidate->egress_link,
                                                new_link);
            candidate->egress_link = new_link;
            candidate->verified_rtt_valid = false;
            candidate->verified_rtt_ms = 0U;
        }
    }
#endif
}

/*
 * EN: Derives `bearer_index_from_entry` with the canonical Lite/Full Node conversion rules.
 * 中文：按照规范的 Lite/Full Node 转换规则推导 `bearer_index_from_entry`。
 */
static size_t bearer_index_from_entry(const ucn_neighbor_entry_t *entry,
                                      const ucn_neighbor_bearer_t *bearer)
{
    return (size_t)(bearer - entry->bearers);
}

/*
 * EN: Checks the `bearer_is_active` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `bearer_is_active` 条件。
 */
static bool bearer_is_active(const ucn_neighbor_bearer_t *bearer)
{
    return bearer != NULL && (bearer->state == UCN_NEIGHBOR_BEARER_ADMITTED ||
                               bearer->state == UCN_NEIGHBOR_BEARER_SUSPECT);
}

/*
 * EN: Selects or resolves `select_neighbor_bearer` using deterministic Lite/Full Node rules.
 * 中文：按照确定性的 Lite/Full Node 规则选择或解析 `select_neighbor_bearer`。
 */
static ucn_neighbor_bearer_t *select_neighbor_bearer(ucn_node_t *node,
                                                      ucn_neighbor_entry_t *entry)
{
    size_t index;
    ucn_neighbor_bearer_t *best = NULL;
    uint16_t best_cost = UCN_LINK_ROUTE_COST_UNKNOWN;
    bool best_cost_known = false;

    if (entry == NULL) {
        return NULL;
    }
    if (entry->primary_bearer_index != UCN_NEIGHBOR_PRIMARY_BEARER_NONE &&
        entry->primary_bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR) {
        ucn_neighbor_bearer_t *primary =
            &entry->bearers[entry->primary_bearer_index];

        bool cost_known;
        uint16_t cost;

        /* A SUSPECT Primary remains usable only when no admitted sibling is
         * available.  Prefer a confirmed Backup immediately at the suspect
         * boundary; the later fallback loop still preserves single-Bearer
         * connectivity during the bounded grace window. */
        if (primary->state == UCN_NEIGHBOR_BEARER_ADMITTED &&
            link_local_select_cost(node, primary->link, &cost_known, &cost)) {
            remap_neighbor_egress_references(node, entry, primary->link);
            return primary;
        }
    }
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        ucn_neighbor_bearer_t *bearer = &entry->bearers[index];
        bool cost_known;
        uint16_t cost;

        if (bearer->state != UCN_NEIGHBOR_BEARER_ADMITTED ||
            !link_local_select_cost(node, bearer->link, &cost_known, &cost)) {
            continue;
        }
        if (best == NULL ||
            (cost_known && (!best_cost_known || cost < best_cost)) ||
            (cost_known == best_cost_known && cost == best_cost &&
             bearer->link->link_id < best->link->link_id)) {
            best = bearer;
            best_cost = cost;
            best_cost_known = cost_known;
        }
    }
    if (best != NULL) {
        entry->primary_bearer_index = (uint8_t)bearer_index_from_entry(entry, best);
        remap_neighbor_egress_references(node, entry, best->link);
        return best;
    }
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        ucn_neighbor_bearer_t *bearer = &entry->bearers[index];
        bool cost_known;
        uint16_t cost;

        if (bearer->state == UCN_NEIGHBOR_BEARER_SUSPECT &&
            link_local_select_cost(node, bearer->link, &cost_known, &cost)) {
            entry->primary_bearer_index = (uint8_t)index;
            remap_neighbor_egress_references(node, entry, bearer->link);
            return bearer;
        }
    }
    entry->primary_bearer_index = UCN_NEIGHBOR_PRIMARY_BEARER_NONE;
    return NULL;
}

/* Quality switching deliberately stays inside one Neighbor's fixed Bearer
 * set.  It never changes an end-to-end route and therefore does not share
 * PATH_PROBE's multi-hop control plane. */
/*
 * EN: Resets `bearer_quality_probe` to its canonical Lite/Full Node state.
 * 中文：把 `bearer_quality_probe` 重置为规范的 Lite/Full Node 状态。
 */
static void reset_bearer_quality_probe(ucn_neighbor_bearer_t *bearer)
{
    if (bearer == NULL) {
        return;
    }
    bearer->quality_probe_id = 0U;
    bearer->quality_probe_sent_at_ms = 0U;
    bearer->quality_better_samples = 0U;
    bearer->quality_probes_sent = 0U;
    bearer->quality_probe_acks = 0U;
    bearer->quality_probe_pending = false;
}

/*
 * EN: Resets `neighbor_quality_probes` to its canonical Lite/Full Node state.
 * 中文：把 `neighbor_quality_probes` 重置为规范的 Lite/Full Node 状态。
 */
static void reset_neighbor_quality_probes(ucn_neighbor_entry_t *entry)
{
    size_t index;

    if (entry == NULL) {
        return;
    }
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        reset_bearer_quality_probe(&entry->bearers[index]);
    }
}

/*
 * EN: Searches bounded Lite/Full Node state for `better_neighbor_bearer`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `better_neighbor_bearer`。
 */
static ucn_neighbor_bearer_t *find_better_neighbor_bearer(
    ucn_node_t *node,
    ucn_neighbor_entry_t *entry,
    const ucn_neighbor_bearer_t *primary)
{
    size_t index;
    ucn_neighbor_bearer_t *best = NULL;
    uint16_t primary_cost;
    uint16_t best_cost = UCN_LINK_ROUTE_COST_UNKNOWN;
    bool primary_cost_known;
    bool best_cost_known = false;

    if (entry == NULL || primary == NULL ||
        !link_local_select_cost(node, primary->link, &primary_cost_known,
                                &primary_cost)) {
        return NULL;
    }
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        ucn_neighbor_bearer_t *bearer = &entry->bearers[index];
        uint16_t bearer_cost;
        bool bearer_cost_known;

        if (bearer == primary ||
            bearer->state != UCN_NEIGHBOR_BEARER_ADMITTED ||
            !link_local_select_cost(node, bearer->link, &bearer_cost_known,
                                    &bearer_cost)) {
            continue;
        }
        if (!bearer_cost_known ||
            (primary_cost_known &&
             !cost_is_sufficiently_better(
                 primary_cost, bearer_cost,
                 UCN_BEARER_SWITCH_IMPROVEMENT_PERCENT))) {
            continue;
        }
        if (best == NULL || !best_cost_known || bearer_cost < best_cost ||
            (bearer_cost == best_cost &&
             bearer->link->link_id < best->link->link_id)) {
            best = bearer;
            best_cost = bearer_cost;
            best_cost_known = true;
        }
    }
    return best;
}

/*
 * EN: Atomically switches a neighbor to a verified primary Bearer and remaps dependents.
 * 中文：把邻居原子切换到已验证的主 Bearer，并重映射依赖项。
 */
static void switch_neighbor_primary(ucn_node_t *node,
                                    ucn_neighbor_entry_t *entry,
                                    ucn_neighbor_bearer_t *bearer)
{
    uint8_t bearer_index;

    if (node == NULL || entry == NULL || bearer == NULL) {
        return;
    }
    bearer_index = (uint8_t)bearer_index_from_entry(entry, bearer);
    if (entry->primary_bearer_index != bearer_index) {
        entry->primary_bearer_index = bearer_index;
        remap_neighbor_egress_references(node, entry, bearer->link);
        entry->bearer_quality_hold_active = true;
        entry->bearer_quality_hold_until_ms = ucn_deadline_from_now(
            node->now_ms, UCN_BEARER_QUALITY_SWITCH_HOLD_MS);
        node->stats.bearer_quality_switches++;
    }
    reset_neighbor_quality_probes(entry);
}

/*
 * EN: Calculates `evaluate_bearer_quality` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `evaluate_bearer_quality`。
 */
static void evaluate_bearer_quality(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        ucn_neighbor_entry_t *entry = &node->neighbors[index];
        ucn_neighbor_bearer_t *primary;
        ucn_neighbor_bearer_t *candidate;
        size_t bearer_index;

        if (entry->state != UCN_NEIGHBOR_ADMITTED &&
            entry->state != UCN_NEIGHBOR_SUSPECT) {
            continue;
        }
        primary = select_neighbor_bearer(node, entry);
        if (primary == NULL) {
            reset_neighbor_quality_probes(entry);
            continue;
        }
        if (entry->bearer_quality_hold_active) {
            if (!ucn_deadline_expired(now_ms,
                                      entry->bearer_quality_hold_until_ms)) {
                reset_neighbor_quality_probes(entry);
                continue;
            }
            entry->bearer_quality_hold_active = false;
        }
        candidate = find_better_neighbor_bearer(node, entry, primary);
        for (bearer_index = 0U;
             bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR;
             ++bearer_index) {
            ucn_neighbor_bearer_t *bearer = &entry->bearers[bearer_index];

            if (bearer != candidate) {
                reset_bearer_quality_probe(bearer);
            }
        }
        if (candidate == NULL ||
            (entry->bearer_quality_sampled &&
             (uint32_t)(now_ms - entry->last_bearer_quality_sample_ms) <
             UCN_BEARER_QUALITY_SAMPLE_INTERVAL_MS)) {
            continue;
        }
        entry->last_bearer_quality_sample_ms = now_ms;
        entry->bearer_quality_sampled = true;
        if (candidate->quality_better_samples <
            UCN_BEARER_QUALITY_STABLE_SAMPLES) {
            candidate->quality_better_samples++;
        }
        if (candidate->quality_better_samples <
            UCN_BEARER_QUALITY_STABLE_SAMPLES) {
            continue;
        }
        if (candidate->quality_probe_acks >=
            UCN_BEARER_QUALITY_PROBE_REQUIRED_ACKS) {
            switch_neighbor_primary(node, entry, candidate);
        } else if (candidate->quality_probes_sent >=
                   UCN_BEARER_QUALITY_PROBE_MAX_ATTEMPTS &&
                   (!candidate->quality_probe_pending ||
                    (uint32_t)(now_ms - candidate->quality_probe_sent_at_ms) >=
                    UCN_BEARER_QUALITY_PROBE_INTERVAL_MS)) {
            reset_bearer_quality_probe(candidate);
        }
    }
}

/* Routes retain the Link that learned them, while a Neighbor may later move
 * its active physical carrier to a healthy backup.  Resolve at send time so
 * that Bearer failover does not invalidate the logical next hop. */
/*
 * EN: Selects or resolves `resolve_egress_link` using deterministic Lite/Full Node rules.
 * 中文：按照确定性的 Lite/Full Node 规则选择或解析 `resolve_egress_link`。
 */
static ucn_link_t *resolve_egress_link(ucn_node_t *node, ucn_link_t *link)
{
    ucn_neighbor_entry_t *entry = find_neighbor_by_link(node, link);
    ucn_neighbor_bearer_t *bearer;

    if (entry == NULL) {
        return link;
    }
    bearer = select_neighbor_bearer(node, entry);
    return bearer == NULL ? NULL : bearer->link;
}

#if UCN_FEATURE_PATH
typedef struct ucn_path_effective_capability {
    ucn_wire_profile_t maximum_wire_profile;
    size_t minimum_mtu;
} ucn_path_effective_capability_t;

/*
 * EN: Reduces a Path capability to the bottleneck imposed by one additional Link.
 * 中文：根据新增 Link 的瓶颈收缩 Path 能力。
 */
static ucn_result_t include_link_in_path_capability(
    const ucn_link_t *link,
    ucn_path_effective_capability_t *capability,
    bool *included)
{
    ucn_link_status_t status;
    ucn_wire_profile_t maximum_profile;
    size_t mtu;
    ucn_result_t result;

    if (link == NULL || capability == NULL || included == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = get_link_status(link, &status);
    if (result != UCN_OK || !status.is_up) {
        return result == UCN_OK ? UCN_ERR_LINK_DOWN : result;
    }
    mtu = ucn_link_effective_mtu(link, &status);
    if (mtu == 0U) {
        return UCN_ERR_LINK_DOWN;
    }
    maximum_profile = link->peer_wire_profile == UCN_WIRE_PROFILE_UNSPECIFIED ?
        UCN_WIRE_PROFILE_W3_BACKBONE : link->peer_wire_profile;
    if (ucn_wire_profile_get_descriptor(maximum_profile) == NULL) {
        return UCN_ERR_CONFIG;
    }
    if (!*included) {
        capability->maximum_wire_profile = maximum_profile;
        capability->minimum_mtu = mtu;
        *included = true;
    } else {
        if (maximum_profile < capability->maximum_wire_profile) {
            capability->maximum_wire_profile = maximum_profile;
        }
        if (mtu < capability->minimum_mtu) {
            capability->minimum_mtu = mtu;
        }
    }
    return UCN_OK;
}

/* Build the failover-safe intersection of every currently eligible Bearer
 * for one logical next hop.  A non-Neighbor Link is a one-member set. */
/*
 * EN: Selects or resolves `resolve_path_bearer_capability` using deterministic Lite/Full Node rules.
 * 中文：按照确定性的 Lite/Full Node 规则选择或解析 `resolve_path_bearer_capability`。
 */
static ucn_result_t resolve_path_bearer_capability(
    ucn_node_t *node,
    ucn_link_t *configured_link,
    ucn_path_effective_capability_t *capability)
{
    ucn_neighbor_entry_t *entry;
    bool included = false;
    size_t index;

    if (node == NULL || configured_link == NULL || capability == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    entry = find_neighbor_by_link(node, configured_link);
    if (entry == NULL) {
        return include_link_in_path_capability(configured_link, capability,
                                               &included);
    }
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        ucn_neighbor_bearer_t *bearer = &entry->bearers[index];
        ucn_result_t result;

        if (bearer->state != UCN_NEIGHBOR_BEARER_ADMITTED &&
            bearer->state != UCN_NEIGHBOR_BEARER_SUSPECT) {
            continue;
        }
        result = include_link_in_path_capability(bearer->link, capability,
                                                 &included);
        if (result != UCN_OK && result != UCN_ERR_LINK_DOWN) {
            return result;
        }
    }
    return included ? UCN_OK : UCN_ERR_LINK_DOWN;
}

/*
 * EN: Selects or resolves `resolve_path_effective_capability` using deterministic Lite/Full Node rules.
 * 中文：按照确定性的 Lite/Full Node 规则选择或解析 `resolve_path_effective_capability`。
 */
static ucn_result_t resolve_path_effective_capability(
    ucn_node_t *node,
    const ucn_path_forward_entry_t *path,
    ucn_path_effective_capability_t *capability)
{
    ucn_result_t result;

    if (node == NULL || path == NULL || path->egress_link == NULL ||
        capability == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = resolve_path_bearer_capability(node, path->egress_link, capability);
    if (result != UCN_OK) {
        return result;
    }
    if (path->minimum_mtu != 0U) {
        const ucn_wire_profile_t stored_profile =
            (ucn_wire_profile_t)path->maximum_wire_profile;

        if (ucn_wire_profile_get_descriptor(stored_profile) == NULL) {
            return UCN_ERR_CONFIG;
        }
        if (stored_profile < capability->maximum_wire_profile) {
            capability->maximum_wire_profile = stored_profile;
        }
        if ((size_t)path->minimum_mtu < capability->minimum_mtu) {
            capability->minimum_mtu = path->minimum_mtu;
        }
    }
    return UCN_OK;
}

/*
 * EN: Checks the `path_result_is_capability_failure` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `path_result_is_capability_failure` 条件。
 */
static bool path_result_is_capability_failure(ucn_result_t result)
{
    return result == UCN_ERR_UNSUPPORTED || result == UCN_ERR_TOO_LARGE;
}
#endif

/* A Path stores the Link through which it was provisioned, but that Link can
 * be one Bearer in a logical Neighbor.  Revoke only after the whole Neighbor
 * Bearer set is unavailable; a primary-to-backup switch must preserve the
 * authenticated Path ID and its forwarding entry. */
#if UCN_FEATURE_PATH
/*
 * EN: Removes or releases `revoke_path_and_mark_local_policy` from Lite/Full Node state with bounded work.
 * 中文：以有界工作量从 Lite/Full Node 状态移除或释放 `revoke_path_and_mark_local_policy`。
 */
static void revoke_path_and_mark_local_policy(ucn_node_t *node,
                                               ucn_node_id_t owner,
                                               ucn_session_id_t owner_session_id,
                                               ucn_path_id_t path_id,
                                               ucn_node_id_t destination)
{
    size_t index;

    if (node == NULL || owner == 0U || owner_session_id == 0U ||
        path_id == 0U || destination == 0U) {
        return;
    }
    (void)ucn_path_revoke(&node->path_state, owner, owner_session_id,
                          path_id, destination);
    if (owner != node->config.node_id || owner_session_id != node->session_id) {
        return;
    }
    for (index = 0U; index < UCN_MAX_POLICY_PATHS; ++index) {
        const ucn_policy_path_entry_t *policy_path =
            &node->policy_state.paths[index];

        if (policy_path->occupied && policy_path->wire_path_id == path_id &&
            policy_path->destination == destination) {
            ucn_policy_mark_path_down(&node->policy_state,
                                      policy_path->local_path_id);
        }
    }
}

/*
 * EN: Removes or releases `revoke_paths_by_unavailable_egress` from Lite/Full Node state with bounded work.
 * 中文：以有界工作量从 Lite/Full Node 状态移除或释放 `revoke_paths_by_unavailable_egress`。
 */
static void revoke_paths_by_unavailable_egress(ucn_node_t *node,
                                                ucn_link_t *failed_link)
{
    ucn_neighbor_entry_t *neighbor;
    size_t index;

    if (node == NULL || failed_link == NULL) {
        return;
    }
    neighbor = find_neighbor_by_link(node, failed_link);
    if (neighbor != NULL && select_neighbor_bearer(node, neighbor) != NULL) {
        return;
    }
    for (index = 0U; index < UCN_MAX_PATH_FORWARD_ENTRIES; ++index) {
        const ucn_path_forward_entry_t *path = &node->path_state.entries[index];
        bool affected = false;

        if (!path->occupied || path->terminal || path->egress_link == NULL) {
            continue;
        }
        if (neighbor != NULL) {
            affected = find_neighbor_bearer(neighbor, path->egress_link) != NULL;
        } else {
            affected = path->egress_link == failed_link &&
                       !link_is_usable(path->egress_link);
        }
        if (affected) {
            revoke_path_and_mark_local_policy(node, path->owner,
                                               path->owner_session_id,
                                               path->path_id,
                                               path->destination);
        }
    }
}
#endif

/* Static Links have no Neighbor entry.  A dynamically admitted Link may
 * carry its existing active traffic during SUSPECT, but it must not be used
 * to construct or validate a new candidate path. */
#if UCN_FEATURE_CANDIDATE_ROUTING || UCN_FEATURE_DIAGNOSTICS
/*
 * EN: Checks the `link_is_candidate_eligible` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `link_is_candidate_eligible` 条件。
 */
static bool link_is_candidate_eligible(ucn_node_t *node, const ucn_link_t *link)
{
    ucn_neighbor_entry_t *entry = find_neighbor_by_link(node, link);
    ucn_neighbor_bearer_t *bearer;

    if (entry == NULL) {
        return true;
    }
    bearer = find_neighbor_bearer(entry, link);
    return bearer != NULL && bearer->state == UCN_NEIGHBOR_BEARER_ADMITTED &&
           select_neighbor_bearer(node, entry) == bearer;
}
#endif

/*
 * EN: Searches bounded Lite/Full Node state for `endpoint_handler`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `endpoint_handler`。
 */
static ucn_endpoint_handler_entry_t *find_endpoint_handler(ucn_node_t *node,
                                                            ucn_endpoint_t endpoint)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ENDPOINT_HANDLERS; ++index) {
        if (node->endpoint_handlers[index].occupied &&
            node->endpoint_handlers[index].endpoint == endpoint) {
            return &node->endpoint_handlers[index];
        }
    }
    return NULL;
}

/*
 * EN: Checks whether `security_policy` satisfies the Lite/Full Node module's validity rules.
 * 中文：检查 `security_policy` 是否满足 Lite/Full Node 模块的合法性规则。
 */
static bool security_policy_is_valid(const ucn_security_policy_t *policy)
{
    return policy != NULL && policy->tx_mode <= UCN_SECURITY_TX_AUTO &&
           policy->rx_mode <= UCN_SECURITY_RX_BOTH &&
           policy->forward_mode <= UCN_SECURITY_FORWARD_TERMINAL_ONLY;
}

/*
 * EN: Checks the current `security_policy_is_production_ready` condition in Lite/Full Node state.
 * 中文：检查当前 Lite/Full Node 状态中的 `security_policy_is_production_ready` 条件。
 */
static bool security_policy_is_production_ready(
    const ucn_security_policy_t *policy)
{
    return security_policy_is_valid(policy) &&
           policy->tx_mode != UCN_SECURITY_TX_PLAIN &&
           policy->rx_mode == UCN_SECURITY_RX_ENCRYPTED_ONLY &&
           policy->forward_mode !=
               UCN_SECURITY_FORWARD_PLAIN_AND_OPAQUE_E2E;
}

/*
 * EN: Searches bounded Lite/Full Node state for `endpoint_security_policy`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `endpoint_security_policy`。
 */
static ucn_endpoint_security_policy_entry_t *find_endpoint_security_policy(
    ucn_node_t *node,
    ucn_endpoint_t endpoint)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ENDPOINT_SECURITY_POLICIES; ++index) {
        if (node->endpoint_security_policies[index].occupied &&
            node->endpoint_security_policies[index].endpoint == endpoint) {
            return &node->endpoint_security_policies[index];
        }
    }
    return NULL;
}

/*
 * EN: Selects or resolves `resolve_security_policy` using deterministic Lite/Full Node rules.
 * 中文：按照确定性的 Lite/Full Node 规则选择或解析 `resolve_security_policy`。
 */
static const ucn_security_policy_t *resolve_security_policy(ucn_node_t *node,
                                                             uint8_t message_type)
{
    ucn_endpoint_security_policy_entry_t *entry;

    if (!ucn_endpoint_is_static((ucn_endpoint_t)message_type)) {
        return &node->security_policy;
    }
    entry = find_endpoint_security_policy(node, (ucn_endpoint_t)message_type);
    return entry == NULL ? &node->security_policy : &entry->policy;
}

/*
 * EN: Validates and processes `dispatch_endpoint` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `dispatch_endpoint`。
 */
static bool dispatch_endpoint(ucn_node_t *node, const ucn_frame_t *frame)
{
    ucn_endpoint_handler_entry_t *entry;

    if (!ucn_endpoint_is_static((ucn_endpoint_t)frame->message_type)) {
        return false;
    }
    entry = find_endpoint_handler(node, (ucn_endpoint_t)frame->message_type);
    if (entry == NULL || entry->handler == NULL) {
        return false;
    }
    entry->handler(entry->context, frame);
    return true;
}

typedef enum ucn_rreq_cache_classification {
    UCN_RREQ_CACHE_NEW = 0,
    UCN_RREQ_CACHE_BETTER,
    UCN_RREQ_CACHE_REPLAY,
    UCN_RREQ_CACHE_FULL
} ucn_rreq_cache_classification_t;

/*
 * EN: Builds and submits `classify_route_request` through the bounded Lite/Full Node transmit path.
 * 中文：构造 `classify_route_request` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_rreq_cache_classification_t classify_route_request(
    ucn_node_t *node,
    const ucn_frame_t *frame,
    ucn_route_cost_t route_cost,
    size_t *slot_index)
{
    const uint32_t request_id =
        read_u32_be(frame->payload + route_request_id_offset(frame));
    size_t reusable_index = UCN_RREQ_CACHE_SIZE;
    size_t index;

    for (index = 0U; index < UCN_RREQ_CACHE_SIZE; ++index) {
        const ucn_rreq_cache_entry_t *slot = &node->rreq_cache[index];

        if (slot->valid && slot->origin == frame->source &&
            slot->session_id == frame->session_id &&
            slot->request_id == request_id) {
            *slot_index = index;
            return route_cost < slot->best_route_request_cost ?
                   UCN_RREQ_CACHE_BETTER : UCN_RREQ_CACHE_REPLAY;
        }
        if ((!slot->valid ||
             ucn_elapsed_at_least(node->now_ms, slot->last_observed_ms,
                                  UCN_RREQ_CACHE_TIMEOUT_MS)) &&
            reusable_index == UCN_RREQ_CACHE_SIZE) {
            reusable_index = index;
        }
    }

    if (reusable_index == UCN_RREQ_CACHE_SIZE) {
        return UCN_RREQ_CACHE_FULL;
    }
    *slot_index = reusable_index;
    return UCN_RREQ_CACHE_NEW;
}

/*
 * EN: Applies `commit_route_request` after validating the current Lite/Full Node state.
 * 中文：验证当前 Lite/Full Node 状态后应用 `commit_route_request`。
 */
static void commit_route_request(ucn_node_t *node,
                                 const ucn_frame_t *frame,
                                 ucn_route_cost_t route_cost,
                                 size_t slot_index)
{
    ucn_rreq_cache_entry_t *slot = &node->rreq_cache[slot_index];

    slot->valid = true;
    slot->origin = frame->source;
    slot->session_id = frame->session_id;
    slot->request_id = read_u32_be(
        frame->payload + route_request_id_offset(frame));
    slot->best_route_request_cost = route_cost;
    slot->last_observed_ms = node->now_ms;
}

/*
 * EN: Removes and returns `take_control_token` from a bounded Lite/Full Node queue or slot.
 * 中文：从固定容量的 Lite/Full Node 队列或槽位中移除并返回 `take_control_token`。
 */
static bool take_control_token(ucn_node_t *node)
{
    uint32_t elapsed = node->now_ms - node->control_last_refill_ms;
    uint32_t refill_count = elapsed / UCN_CONTROL_TOKEN_REFILL_MS;

    if (refill_count != 0U) {
        uint32_t new_tokens = (uint32_t)node->control_tokens + refill_count;

        node->control_tokens = (uint8_t)(new_tokens > UCN_CONTROL_TOKEN_BURST ?
                                         UCN_CONTROL_TOKEN_BURST : new_tokens);
        node->control_last_refill_ms += refill_count * UCN_CONTROL_TOKEN_REFILL_MS;
    }
    if (node->control_tokens == 0U) {
        node->stats.control_budget_dropped++;
        return false;
    }
    --node->control_tokens;
    return true;
}

/*
 * EN: Records `note_control_rx_drop` in bounded Lite/Full Node state or statistics.
 * 中文：在固定容量的 Lite/Full Node 状态或统计中记录 `note_control_rx_drop`。
 */
static void note_control_rx_drop(ucn_node_t *node,
                                 ucn_control_rx_budget_type_t type)
{
    if (type == UCN_CONTROL_RX_ROUTE_REQUEST) {
        node->stats.route_request_rx_rate_dropped++;
    } else if (type == UCN_CONTROL_RX_HEARTBEAT_REQUEST) {
        node->stats.heartbeat_rx_rate_dropped++;
    } else if (type == UCN_CONTROL_RX_PATH_TRACE_REQUEST) {
        node->stats.path_trace_rx_rate_dropped++;
    }
}

/*
 * EN: Searches bounded Lite/Full Node state for `control_rx_peer_budget`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `control_rx_peer_budget`。
 */
static ucn_control_rx_peer_budget_t *find_control_rx_peer_budget(
    ucn_node_t *node,
    ucn_node_id_t peer_node_id,
    bool allocate)
{
    ucn_control_rx_peer_budget_t *free_slot = NULL;
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        ucn_control_rx_peer_budget_t *slot = &node->control_rx_peer_budgets[index];

        if (slot->occupied && slot->peer_node_id == peer_node_id) {
            return slot;
        }
        if (!slot->occupied && free_slot == NULL) {
            free_slot = slot;
        }
    }
    if (!allocate || free_slot == NULL) {
        return NULL;
    }
    (void)memset(free_slot, 0, sizeof(*free_slot));
    free_slot->occupied = true;
    free_slot->peer_node_id = peer_node_id;
    free_slot->budgets[UCN_CONTROL_RX_ROUTE_REQUEST].tokens =
        UCN_ROUTE_REQUEST_RX_TOKEN_BURST;
    free_slot->budgets[UCN_CONTROL_RX_HEARTBEAT_REQUEST].tokens =
        UCN_HEARTBEAT_RX_TOKEN_BURST;
    free_slot->budgets[UCN_CONTROL_RX_PATH_TRACE_REQUEST].tokens =
        UCN_PATH_TRACE_RX_TOKEN_BURST;
    free_slot->budgets[UCN_CONTROL_RX_ROUTE_REQUEST].last_refill_ms = node->now_ms;
    free_slot->budgets[UCN_CONTROL_RX_HEARTBEAT_REQUEST].last_refill_ms = node->now_ms;
    free_slot->budgets[UCN_CONTROL_RX_PATH_TRACE_REQUEST].last_refill_ms = node->now_ms;
    return free_slot;
}

/*
 * EN: Removes or releases `release_control_rx_peer_budget` from Lite/Full Node state with bounded work.
 * 中文：以有界工作量从 Lite/Full Node 状态移除或释放 `release_control_rx_peer_budget`。
 */
static void release_control_rx_peer_budget(ucn_node_t *node,
                                           ucn_node_id_t peer_node_id)
{
    ucn_control_rx_peer_budget_t *slot =
        find_control_rx_peer_budget(node, peer_node_id, false);

    if (slot != NULL) {
        (void)memset(slot, 0, sizeof(*slot));
    }
}

/*
 * EN: Removes and returns `take_control_rx_token` from a bounded Lite/Full Node queue or slot.
 * 中文：从固定容量的 Lite/Full Node 队列或槽位中移除并返回 `take_control_rx_token`。
 */
static bool take_control_rx_token(ucn_node_t *node,
                                  const ucn_link_t *ingress_link,
                                  ucn_control_rx_budget_type_t type)
{
    ucn_control_rx_peer_budget_t *peer_budget;
    ucn_control_rx_budget_t *budget;
    uint8_t burst;
    uint32_t refill_ms;
    uint32_t elapsed;
    uint32_t refill_count;

    if (type >= UCN_CONTROL_RX_BUDGET_TYPE_COUNT || ingress_link == NULL ||
        ingress_link->peer_node_id == 0U ||
        ingress_link->peer_node_id == UCN_NODE_BROADCAST) {
        return false;
    }
    peer_budget = find_control_rx_peer_budget(node,
                                               ingress_link->peer_node_id, true);
    if (peer_budget == NULL) {
        note_control_rx_drop(node, type);
        return false;
    }
    budget = &peer_budget->budgets[type];
    switch (type) {
    case UCN_CONTROL_RX_ROUTE_REQUEST:
        burst = UCN_ROUTE_REQUEST_RX_TOKEN_BURST;
        refill_ms = UCN_ROUTE_REQUEST_RX_TOKEN_REFILL_MS;
        break;
    case UCN_CONTROL_RX_HEARTBEAT_REQUEST:
        burst = UCN_HEARTBEAT_RX_TOKEN_BURST;
        refill_ms = UCN_HEARTBEAT_RX_TOKEN_REFILL_MS;
        break;
    case UCN_CONTROL_RX_PATH_TRACE_REQUEST:
        burst = UCN_PATH_TRACE_RX_TOKEN_BURST;
        refill_ms = UCN_PATH_TRACE_RX_TOKEN_REFILL_MS;
        break;
    default:
        return false;
    }

    elapsed = node->now_ms - budget->last_refill_ms;
    refill_count = elapsed / refill_ms;
    if (refill_count != 0U) {
        const uint32_t new_tokens = (uint32_t)budget->tokens + refill_count;

        budget->tokens = (uint8_t)(new_tokens > burst ? burst : new_tokens);
        budget->last_refill_ms += refill_count * refill_ms;
    }
    if (budget->tokens == 0U) {
        note_control_rx_drop(node, type);
        return false;
    }
    --budget->tokens;
    return true;
}

/* Diagnostic traffic has an independent, much smaller budget.  A manual
 * topology query can therefore never consume the Q0 control budget used by
 * join, liveness, and route recovery. */
#if UCN_FEATURE_DIAGNOSTICS
/*
 * EN: Removes and returns `take_path_trace_token` from a bounded Lite/Full Node queue or slot.
 * 中文：从固定容量的 Lite/Full Node 队列或槽位中移除并返回 `take_path_trace_token`。
 */
static bool take_path_trace_token(ucn_node_t *node)
{
    uint32_t elapsed = node->now_ms - node->path_trace_last_refill_ms;
    uint32_t refill_count = elapsed / UCN_PATH_TRACE_TOKEN_REFILL_MS;

    if (refill_count != 0U) {
        uint32_t new_tokens = (uint32_t)node->path_trace_tokens + refill_count;

        node->path_trace_tokens =
            (uint8_t)(new_tokens > UCN_PATH_TRACE_TOKEN_BURST ?
                          UCN_PATH_TRACE_TOKEN_BURST : new_tokens);
        node->path_trace_last_refill_ms +=
            refill_count * UCN_PATH_TRACE_TOKEN_REFILL_MS;
    }
    if (node->path_trace_tokens == 0U) {
        node->stats.path_trace_rate_dropped++;
        return false;
    }
    --node->path_trace_tokens;
    return true;
}

/* A snapshot touches the whole reachable component, unlike a single-path
 * trace.  It therefore has its own, slower token bucket. */
/*
 * EN: Removes and returns `take_node_snapshot_token` from a bounded Lite/Full Node queue or slot.
 * 中文：从固定容量的 Lite/Full Node 队列或槽位中移除并返回 `take_node_snapshot_token`。
 */
static bool take_node_snapshot_token(ucn_node_t *node)
{
    uint32_t elapsed = node->now_ms - node->node_snapshot_last_refill_ms;
    uint32_t refill_count = elapsed / UCN_NODE_SNAPSHOT_TOKEN_REFILL_MS;

    if (refill_count != 0U) {
        uint32_t new_tokens = (uint32_t)node->node_snapshot_tokens + refill_count;

        node->node_snapshot_tokens =
            (uint8_t)(new_tokens > UCN_NODE_SNAPSHOT_TOKEN_BURST ?
                          UCN_NODE_SNAPSHOT_TOKEN_BURST : new_tokens);
        node->node_snapshot_last_refill_ms +=
            refill_count * UCN_NODE_SNAPSHOT_TOKEN_REFILL_MS;
    }
    if (node->node_snapshot_tokens == 0U) {
        node->stats.node_snapshot_rate_dropped++;
        return false;
    }
    --node->node_snapshot_tokens;
    return true;
}

/* Per-node strategy inspection is unicast, but still must never consume the
 * Q0 control budget or become a periodic telemetry stream. */
/*
 * EN: Removes and returns `take_policy_diagnostic_token` from a bounded Lite/Full Node queue or slot.
 * 中文：从固定容量的 Lite/Full Node 队列或槽位中移除并返回 `take_policy_diagnostic_token`。
 */
static bool take_policy_diagnostic_token(ucn_node_t *node)
{
    uint32_t elapsed = node->now_ms - node->policy_diagnostic_last_refill_ms;
    uint32_t refill_count = elapsed / UCN_POLICY_DIAGNOSTIC_TOKEN_REFILL_MS;

    if (refill_count != 0U) {
        uint32_t new_tokens = (uint32_t)node->policy_diagnostic_tokens + refill_count;

        node->policy_diagnostic_tokens =
            (uint8_t)(new_tokens > UCN_POLICY_DIAGNOSTIC_TOKEN_BURST ?
                          UCN_POLICY_DIAGNOSTIC_TOKEN_BURST : new_tokens);
        node->policy_diagnostic_last_refill_ms +=
            refill_count * UCN_POLICY_DIAGNOSTIC_TOKEN_REFILL_MS;
    }
    if (node->policy_diagnostic_tokens == 0U) {
        node->stats.policy_diagnostic_rate_dropped++;
        return false;
    }
    --node->policy_diagnostic_tokens;
    return true;
}
#endif

/*
 * EN: Checks or removes expired `expire_neighbor_candidates` state in Lite/Full Node.
 * 中文：检查或移除 Lite/Full Node 中已过期的 `expire_neighbor_candidates` 状态。
 */
static void expire_neighbor_candidates(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        ucn_neighbor_entry_t *entry = &node->neighbors[index];
        size_t bearer_index;
        bool has_candidate = false;
        bool has_active = false;

        if (entry->state == UCN_NEIGHBOR_EMPTY) {
            continue;
        }
        for (bearer_index = 0U;
             bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR;
             ++bearer_index) {
            ucn_neighbor_bearer_t *bearer = &entry->bearers[bearer_index];

            if (bearer->state == UCN_NEIGHBOR_BEARER_CANDIDATE &&
                ucn_elapsed_at_least(now_ms, bearer->last_seen_ms,
                                     UCN_NEIGHBOR_CANDIDATE_TIMEOUT_MS)) {
                if (bearer->link != NULL && !link_is_registered(node, bearer->link)) {
                    bearer->link->peer_node_id = 0U;
                }
                (void)memset(bearer, 0, sizeof(*bearer));
                --entry->bearer_count;
                continue;
            }
            has_candidate = has_candidate ||
                bearer->state == UCN_NEIGHBOR_BEARER_CANDIDATE;
            has_active = has_active || bearer_is_active(bearer);
        }
        if (!has_active && !has_candidate && entry->bearer_count == 0U &&
            entry->state == UCN_NEIGHBOR_CANDIDATE) {
            entry->state = UCN_NEIGHBOR_EXPIRED;
        }
    }
}

/*
 * EN: Allocates `allocate_neighbor_slot` from fixed Lite/Full Node slots without heap use.
 * 中文：从 Lite/Full Node 的固定槽位分配 `allocate_neighbor_slot`，不使用堆内存。
 */
static ucn_neighbor_entry_t *allocate_neighbor_slot(ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        if (node->neighbors[index].state == UCN_NEIGHBOR_EMPTY ||
            node->neighbors[index].state == UCN_NEIGHBOR_REMOVED ||
            node->neighbors[index].state == UCN_NEIGHBOR_REJECTED ||
            node->neighbors[index].state == UCN_NEIGHBOR_EXPIRED) {
            return &node->neighbors[index];
        }
    }
    return NULL;
}

/*
 * EN: Validates and installs `admit_neighbor_entry` into bounded Lite/Full Node state.
 * 中文：验证 `admit_neighbor_entry` 并将其安装到固定容量的 Lite/Full Node 状态中。
 */
static ucn_result_t admit_neighbor_entry(ucn_node_t *node,
                                         ucn_neighbor_entry_t *entry)
{
    size_t index;
    ucn_result_t result = UCN_ERR_NOT_FOUND;
    bool admitted = false;

    if (entry == NULL || entry->state == UCN_NEIGHBOR_REJECTED ||
        entry->state == UCN_NEIGHBOR_EXPIRED ||
        entry->state == UCN_NEIGHBOR_REMOVED) {
        return UCN_ERR_NOT_FOUND;
    }
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        ucn_neighbor_bearer_t *bearer = &entry->bearers[index];

        if (bearer->state != UCN_NEIGHBOR_BEARER_CANDIDATE) {
            continue;
        }
        if (link_is_registered(node, bearer->link)) {
            bearer->state = UCN_NEIGHBOR_BEARER_ADMITTED;
            admitted = true;
            continue;
        }
        result = ucn_node_register_link(node, bearer->link);
        if (result != UCN_OK) {
            return result;
        }
        bearer->state = UCN_NEIGHBOR_BEARER_ADMITTED;
        admitted = true;
    }
    if (admitted || entry->state == UCN_NEIGHBOR_ADMITTED ||
        entry->state == UCN_NEIGHBOR_SUSPECT) {
        entry->state = UCN_NEIGHBOR_ADMITTED;
        entry->suspect_since_ms = 0U;
        (void)select_neighbor_bearer(node, entry);
        return UCN_OK;
    }
    return result;
}

/*
 * EN: Clears `discovery` from Lite/Full Node without allocating memory.
 * 中文：从 Lite/Full Node 中清除 `discovery`，且不进行动态分配。
 */
static void clear_discovery(ucn_node_t *node, ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        if (node->discoveries[index].active &&
            node->discoveries[index].destination == destination) {
            node->discoveries[index].active = false;
        }
    }
#if UCN_FEATURE_CANDIDATE_ROUTING
    expire_candidate_routes(node);
#endif
}

/*
 * EN: Checks or removes expired `expire_dynamic_state` state in Lite/Full Node.
 * 中文：检查或移除 Lite/Full Node 中已过期的 `expire_dynamic_state` 状态。
 */
static void expire_dynamic_state(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    node->now_ms = now_ms;
    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid && route_is_expired(node, &node->routes[index])) {
            node->routes[index].valid = false;
        } else if (node->routes[index].previous_valid &&
                   ucn_deadline_expired(now_ms,
                                        node->routes[index].previous_expires_at_ms)) {
            node->routes[index].previous_valid = false;
            node->routes[index].previous_egress_link = NULL;
            node->routes[index].previous_route_epoch = 0U;
            node->routes[index].previous_expires_at_ms = 0U;
        }
    }

    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        if (node->discoveries[index].active &&
            (ucn_elapsed_at_least(
                 now_ms, node->discoveries[index].overall_started_at_ms,
                 UCN_ROUTE_REQUEST_TIMEOUT_MS) ||
             (ucn_deadline_expired(now_ms,
                                   node->discoveries[index].deadline_ms) &&
              node->discoveries[index].current_hop_limit >=
                  node->discoveries[index].maximum_hop_limit))) {
            node->discoveries[index].active = false;
        }
    }
}

/*
 * EN: Updates `learn_route` in bounded Lite/Full Node state.
 * 中文：更新固定容量 Lite/Full Node 状态中的 `learn_route`。
 */
static ucn_result_t learn_route(ucn_node_t *node,
                                ucn_node_id_t destination,
                                ucn_link_t *egress_link,
                                ucn_route_cost_t route_cost,
                                uint8_t hop_count,
                                uint16_t route_epoch)
{
    size_t index;
    ucn_route_entry_t *free_slot = NULL;

    if (hop_count == 0U || hop_count > node->config.default_hop_limit) {
        return UCN_ERR_TTL;
    }
    if (destination == 0U || destination == UCN_NODE_BROADCAST || route_epoch == 0U ||
        egress_link == NULL || !link_is_registered(node, egress_link)) {
        return UCN_ERR_ARGUMENT;
    }

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid &&
            node->routes[index].destination == destination) {
            if (!node->routes[index].is_static) {
                ucn_route_entry_t *route = &node->routes[index];
                const bool lower_cost = route_cost < route->route_cost;
                const bool same_path_refresh =
                    route_cost == route->route_cost &&
                    hop_count == route->hop_count &&
                    egress_link == route->egress_link;

                if (lower_cost || same_path_refresh) {
                    if (same_path_refresh && route_epoch != route->route_epoch) {
                        /* Expanding-ring retries use a fresh request ID and
                         * therefore a fresh epoch. Relays reached by the
                         * smaller ring already hold the same reverse path;
                         * refreshing only its lifetime leaves the returned
                         * forward path on the new epoch and rejects every
                         * business frame at the first relay. Keep the old
                         * epoch briefly for in-flight frames, then publish
                         * the refreshed epoch on the unchanged egress. */
                        route->previous_valid = true;
                        route->previous_egress_link = route->egress_link;
                        route->previous_route_epoch = route->route_epoch;
                        route->previous_expires_at_ms =
                            ucn_deadline_from_now(node->now_ms,
                                                  UCN_ROUTE_EPOCH_GRACE_MS);
                    }
                    route->egress_link = egress_link;
                    route->route_cost = route_cost;
                    route->hop_count = hop_count;
                    route->route_epoch = route_epoch;
                    /* A lower-cost RREP may represent a different downstream
                     * path even when this node keeps the same first hop.  RTT
                     * is end-to-end evidence, so never carry the old sample
                     * across a material Route update. */
                    if (lower_cost) {
                        route->verified_rtt_valid = false;
                        route->verified_rtt_ms = 0U;
                    }
                }
                route->expires_at_ms =
                    ucn_deadline_from_now(node->now_ms,
                                          UCN_ROUTE_ENTRY_LIFETIME_MS);
                route->last_refresh_started_ms = node->now_ms;
            }
            return UCN_OK;
        }
        if (!node->routes[index].valid && free_slot == NULL) {
            free_slot = &node->routes[index];
        }
    }

    if (free_slot == NULL) {
        return UCN_ERR_NO_SPACE;
    }

    free_slot->valid = true;
    free_slot->is_static = false;
    free_slot->destination = destination;
    free_slot->egress_link = egress_link;
    free_slot->expires_at_ms =
        ucn_deadline_from_now(node->now_ms, UCN_ROUTE_ENTRY_LIFETIME_MS);
    free_slot->last_used_at_ms = 0U;
    free_slot->last_refresh_started_ms = node->now_ms;
    free_slot->route_cost = route_cost;
    free_slot->hop_count = hop_count;
    free_slot->verified_rtt_valid = false;
    free_slot->verified_rtt_ms = 0U;
    free_slot->route_epoch = route_epoch;
    free_slot->previous_valid = false;
    free_slot->previous_egress_link = NULL;
    free_slot->previous_route_epoch = 0U;
    free_slot->previous_expires_at_ms = 0U;
    return UCN_OK;
}

/*
 * EN: Searches bounded Lite/Full Node state for `active_route`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `active_route`。
 */
static ucn_route_entry_t *find_active_route(ucn_node_t *node,
                                            ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid &&
            node->routes[index].destination == destination &&
            !route_is_expired(node, &node->routes[index])) {
            return &node->routes[index];
        }
    }
    return NULL;
}

#if UCN_FEATURE_CANDIDATE_ROUTING
/*
 * EN: Searches bounded Lite/Full Node state for `candidate_route`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `candidate_route`。
 */
static ucn_candidate_route_t *find_candidate_route(ucn_node_t *node,
                                                    ucn_node_id_t destination,
                                                    uint32_t candidate_id)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        if (node->candidates[index].valid &&
            node->candidates[index].destination == destination &&
            node->candidates[index].candidate_id == candidate_id &&
            !candidate_is_expired(node, &node->candidates[index])) {
            return &node->candidates[index];
        }
    }
    return NULL;
}

/*
 * EN: Updates `learn_candidate_route` in bounded Lite/Full Node state.
 * 中文：更新固定容量 Lite/Full Node 状态中的 `learn_candidate_route`。
 */
static ucn_result_t learn_candidate_route(ucn_node_t *node,
                                          ucn_node_id_t destination,
                                          uint32_t candidate_id,
                                          ucn_link_t *egress_link,
                                          ucn_route_cost_t route_cost,
                                          uint8_t hop_count,
                                          ucn_wire_profile_t wire_profile,
                                          bool originated_here)
{
    ucn_candidate_route_t *slot;
    size_t index;

    if (hop_count == 0U || hop_count > node->config.default_hop_limit) {
        return UCN_ERR_TTL;
    }
    if (destination == 0U || destination == UCN_NODE_BROADCAST ||
        candidate_id == 0U || egress_link == NULL ||
        !link_is_registered(node, egress_link) ||
        ucn_wire_profile_get_descriptor(wire_profile) == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    slot = find_candidate_route(node, destination, candidate_id);
    if (slot != NULL) {
        if (slot->wire_profile != wire_profile) {
            return UCN_ERR_MALFORMED;
        }
        if (route_cost < slot->route_cost) {
            slot->egress_link = egress_link;
            slot->route_cost = route_cost;
            slot->hop_count = hop_count;
        }
        slot->originated_here = slot->originated_here || originated_here;
        slot->expires_at_ms =
            ucn_deadline_from_now(node->now_ms,
                                  UCN_ROUTE_CANDIDATE_TIMEOUT_MS);
        return UCN_OK;
    }

    slot = NULL;
    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        if (!node->candidates[index].valid ||
            candidate_is_expired(node, &node->candidates[index])) {
            slot = &node->candidates[index];
            break;
        }
    }
    if (slot == NULL) {
        return UCN_ERR_NO_SPACE;
    }

    (void)memset(slot, 0, sizeof(*slot));
    slot->valid = true;
    slot->originated_here = originated_here;
    slot->wire_profile = wire_profile;
    slot->destination = destination;
    slot->candidate_id = candidate_id;
    slot->egress_link = egress_link;
    slot->expires_at_ms =
        ucn_deadline_from_now(node->now_ms, UCN_ROUTE_CANDIDATE_TIMEOUT_MS);
    slot->route_cost = route_cost;
    slot->hop_count = hop_count;
    node->stats.candidate_routes_learned++;
    return UCN_OK;
}

/*
 * EN: Applies `activate_candidate_route` after validating the current Lite/Full Node state.
 * 中文：验证当前 Lite/Full Node 状态后应用 `activate_candidate_route`。
 */
static ucn_result_t activate_candidate_route(ucn_node_t *node,
                                             ucn_node_id_t destination,
                                             uint32_t candidate_id,
                                             uint16_t route_epoch,
                                             ucn_wire_profile_t wire_profile)
{
    ucn_candidate_route_t *candidate;
    const ucn_wire_profile_descriptor_t *descriptor;
    uint16_t maximum_epoch;
    ucn_route_entry_t *route;
    size_t index;

    candidate = find_candidate_route(node, destination, candidate_id);
    descriptor = ucn_wire_profile_get_descriptor(wire_profile);
    maximum_epoch = descriptor != NULL && descriptor->route_epoch_bytes == 1U ?
        UINT16_C(0x00FF) : UINT16_MAX;
    if (candidate == NULL || descriptor == NULL || route_epoch == 0U ||
        candidate->wire_profile != wire_profile ||
        route_epoch >= maximum_epoch) {
        return UCN_ERR_NOT_FOUND;
    }
    route = find_active_route(node, destination);
    if (route != NULL && route->is_static) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (route == NULL) {
        for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
            if (!node->routes[index].valid ||
                route_is_expired(node, &node->routes[index])) {
                route = &node->routes[index];
                break;
            }
        }
    }
    if (route == NULL) {
        return UCN_ERR_NO_SPACE;
    }

    if (route->valid) {
        route->previous_valid = true;
        route->previous_egress_link = route->egress_link;
        route->previous_route_epoch = route->route_epoch;
        route->previous_expires_at_ms =
            ucn_deadline_from_now(node->now_ms, UCN_ROUTE_EPOCH_GRACE_MS);
    } else {
        route->previous_valid = false;
        route->previous_egress_link = NULL;
        route->previous_route_epoch = 0U;
        route->previous_expires_at_ms = 0U;
    }
    route->valid = true;
    route->is_static = false;
    route->destination = destination;
    route->egress_link = candidate->egress_link;
    route->expires_at_ms =
        ucn_deadline_from_now(node->now_ms, UCN_ROUTE_ENTRY_LIFETIME_MS);
    route->last_used_at_ms = 0U;
    route->last_refresh_started_ms = node->now_ms;
    route->route_cost = candidate->route_cost;
    route->hop_count = candidate->hop_count;
    route->verified_rtt_valid = candidate->verified_rtt_valid;
    route->verified_rtt_ms = candidate->verified_rtt_ms;
    route->route_epoch = route_epoch;
    candidate->valid = false;
    return UCN_OK;
}

/*
 * EN: Checks or removes expired `expire_candidate_routes` state in Lite/Full Node.
 * 中文：检查或移除 Lite/Full Node 中已过期的 `expire_candidate_routes` 状态。
 */
static void expire_candidate_routes(ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        if (node->candidates[index].valid &&
            candidate_is_expired(node, &node->candidates[index])) {
            node->candidates[index].valid = false;
        }
    }
}
#endif

/*
 * EN: Updates `mark_route_used` in bounded Lite/Full Node state.
 * 中文：更新固定容量 Lite/Full Node 状态中的 `mark_route_used`。
 */
static void mark_route_used(ucn_node_t *node, ucn_node_id_t destination)
{
    ucn_route_entry_t *route = find_active_route(node, destination);

    if (route != NULL && !route->is_static) {
        route->last_used_at_ms = node->now_ms;
    }
}

#if UCN_FEATURE_CANDIDATE_ROUTING
/*
 * EN: Starts or prepares `start_due_route_refresh` after validating Lite/Full Node prerequisites.
 * 中文：验证 Lite/Full Node 前置条件后启动或准备 `start_due_route_refresh`。
 */
static ucn_result_t start_due_route_refresh(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        ucn_route_entry_t *route = &node->routes[index];

        if (!route->valid || route->is_static || route_is_expired(node, route) ||
            (uint32_t)(now_ms - route->last_used_at_ms) >
            UCN_ROUTE_ENTRY_LIFETIME_MS ||
            (uint32_t)(now_ms - route->last_refresh_started_ms) <
            UCN_ROUTE_REFRESH_MIN_INTERVAL_MS ||
            !ucn_deadline_due_within(now_ms, route->expires_at_ms,
                                     UCN_ROUTE_REFRESH_ADVANCE_MS)) {
            continue;
        }
        return begin_route_discovery(node, route->destination, now_ms, true, 0U,
                                     false, false);
    }
    return UCN_ERR_NOT_FOUND;
}
#endif

/*
 * EN: Clears or releases `invalidate_routes_by_link` from bounded Lite/Full Node state.
 * 中文：从固定容量的 Lite/Full Node 状态中清除或释放 `invalidate_routes_by_link`。
 */
static void invalidate_routes_by_link(ucn_node_t *node, const ucn_link_t *link)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid && !node->routes[index].is_static &&
            node->routes[index].egress_link == link) {
            node->routes[index].valid = false;
            node->routes[index].previous_valid = false;
        } else if (node->routes[index].previous_valid &&
                   node->routes[index].previous_egress_link == link) {
            node->routes[index].previous_valid = false;
            node->routes[index].previous_egress_link = NULL;
        }
    }
#if UCN_FEATURE_CANDIDATE_ROUTING
    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        if (node->candidates[index].valid &&
            node->candidates[index].egress_link == link) {
            node->candidates[index].valid = false;
        }
    }
#endif
}

/*
 * EN: Clears or releases `unregister_link` from bounded Lite/Full Node state.
 * 中文：从固定容量的 Lite/Full Node 状态中清除或释放 `unregister_link`。
 */
static void unregister_link(ucn_node_t *node, ucn_link_t *link)
{
    size_t index;

    for (index = 0U; index < node->link_count; ++index) {
        if (node->links[index] == link) {
            size_t move_index;

            if (link->ops != NULL && link->ops->close != NULL) {
                link->ops->close(link);
            }
            for (move_index = index + 1U; move_index < node->link_count; ++move_index) {
                node->links[move_index - 1U] = node->links[move_index];
            }
            --node->link_count;
            node->links[node->link_count] = NULL;
            link->peer_wire_profile = UCN_WIRE_PROFILE_UNSPECIFIED;
            return;
        }
    }
}

/*
 * EN: Removes or releases `remove_neighbor_entry` from Lite/Full Node state with bounded work.
 * 中文：以有界工作量从 Lite/Full Node 状态移除或释放 `remove_neighbor_entry`。
 */
static void remove_neighbor_entry(ucn_node_t *node, ucn_neighbor_entry_t *entry)
{
    size_t index;

    if (entry == NULL || entry->state == UCN_NEIGHBOR_EMPTY ||
        entry->state == UCN_NEIGHBOR_REMOVED) {
        return;
    }

    /* The entry is still available here, so the helper can distinguish this
     * complete logical-neighbor loss from a single failed physical Bearer. */
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        if (entry->bearers[index].link != NULL) {
#if UCN_FEATURE_PATH
            revoke_paths_by_unavailable_egress(node, entry->bearers[index].link);
#endif
            break;
        }
    }
    release_control_rx_peer_budget(node, entry->peer_node_id);
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        ucn_link_t *link = entry->bearers[index].link;

        if (link == NULL) {
            continue;
        }
        invalidate_routes_by_link(node, link);
        unregister_link(node, link);
        link->peer_node_id = 0U;
    }
    (void)memset(entry, 0, sizeof(*entry));
    entry->state = UCN_NEIGHBOR_REMOVED;
    node->stats.neighbor_removed++;
}

/*
 * EN: Records `touch_neighbor` in bounded Lite/Full Node state or statistics.
 * 中文：在固定容量的 Lite/Full Node 状态或统计中记录 `touch_neighbor`。
 */
static void touch_neighbor(ucn_node_t *node, ucn_link_t *link)
{
    ucn_neighbor_entry_t *entry = find_neighbor_by_link(node, link);
    ucn_neighbor_bearer_t *bearer = find_neighbor_bearer(entry, link);

    if (entry != NULL && bearer != NULL && bearer_is_active(bearer)) {
        bearer->last_seen_ms = node->now_ms;
        if (bearer->state == UCN_NEIGHBOR_BEARER_SUSPECT) {
            bearer->state = UCN_NEIGHBOR_BEARER_ADMITTED;
        }
        if (entry->state == UCN_NEIGHBOR_SUSPECT) {
            entry->state = UCN_NEIGHBOR_ADMITTED;
            entry->suspect_since_ms = 0U;
        }
        (void)select_neighbor_bearer(node, entry);
    }
}

/*
 * EN: Checks the `neighbor_has_active_bearer` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `neighbor_has_active_bearer` 条件。
 */
static bool neighbor_has_active_bearer(const ucn_neighbor_entry_t *entry)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        if (bearer_is_active(&entry->bearers[index])) {
            return true;
        }
    }
    return false;
}

/*
 * EN: Checks the `neighbor_has_admitted_bearer` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `neighbor_has_admitted_bearer` 条件。
 */
static bool neighbor_has_admitted_bearer(const ucn_neighbor_entry_t *entry)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        if (entry->bearers[index].state == UCN_NEIGHBOR_BEARER_ADMITTED) {
            return true;
        }
    }
    return false;
}

/*
 * EN: Updates `refresh_neighbor_liveness_state` in bounded Lite/Full Node state.
 * 中文：更新固定容量 Lite/Full Node 状态中的 `refresh_neighbor_liveness_state`。
 */
static void refresh_neighbor_liveness_state(ucn_node_t *node,
                                             ucn_neighbor_entry_t *entry,
                                             uint32_t now_ms)
{
    if (neighbor_has_admitted_bearer(entry)) {
        entry->state = UCN_NEIGHBOR_ADMITTED;
        entry->suspect_since_ms = 0U;
        (void)select_neighbor_bearer(node, entry);
        return;
    }
    if (neighbor_has_active_bearer(entry)) {
        if (entry->state != UCN_NEIGHBOR_SUSPECT) {
            entry->state = UCN_NEIGHBOR_SUSPECT;
            entry->suspect_since_ms = now_ms;
            node->stats.neighbor_suspected++;
        }
        (void)select_neighbor_bearer(node, entry);
        return;
    }
    remove_neighbor_entry(node, entry);
}

/* A Link may transition down between a caller's selection and the local
 * status check in send_frame_on_link().  Treat that observed local failure
 * like a sampled-down Bearer so a later Path resolution can use its backup. */
/*
 * EN: Updates `mark_neighbor_bearer_down` in bounded Lite/Full Node state.
 * 中文：更新固定容量 Lite/Full Node 状态中的 `mark_neighbor_bearer_down`。
 */
static void mark_neighbor_bearer_down(ucn_node_t *node, ucn_link_t *link)
{
    ucn_neighbor_entry_t *entry = find_neighbor_by_link(node, link);
    ucn_neighbor_bearer_t *bearer = find_neighbor_bearer(entry, link);

    if (entry == NULL || bearer == NULL || !bearer_is_active(bearer)) {
        return;
    }
    bearer->state = UCN_NEIGHBOR_BEARER_DOWN;
    if (entry->primary_bearer_index == bearer_index_from_entry(entry, bearer)) {
        entry->primary_bearer_index = UCN_NEIGHBOR_PRIMARY_BEARER_NONE;
    }
    refresh_neighbor_liveness_state(node, entry, node->now_ms);
}

/*
 * EN: Processes one bounded `maintain_neighbor_liveness` work unit for Lite/Full Node.
 * 中文：为 Lite/Full Node 处理一个有界的 `maintain_neighbor_liveness` 工作单元。
 */
static void maintain_neighbor_liveness(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        ucn_neighbor_entry_t *entry = &node->neighbors[index];
        size_t bearer_index;

        if (entry->state != UCN_NEIGHBOR_ADMITTED &&
            entry->state != UCN_NEIGHBOR_SUSPECT) {
            continue;
        }
        for (bearer_index = 0U;
             bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR;
             ++bearer_index) {
            ucn_neighbor_bearer_t *bearer = &entry->bearers[bearer_index];

            if (!bearer_is_active(bearer)) {
                continue;
            }
            if (!link_is_usable(bearer->link)) {
                bearer->state = UCN_NEIGHBOR_BEARER_DOWN;
                if (entry->primary_bearer_index == bearer_index) {
                    entry->primary_bearer_index = UCN_NEIGHBOR_PRIMARY_BEARER_NONE;
                }
                continue;
            }
            if (bearer->state == UCN_NEIGHBOR_BEARER_ADMITTED &&
                ucn_elapsed_at_least(now_ms, bearer->last_seen_ms,
                                     link_suspect_timeout_ms(bearer->link))) {
                bearer->state = UCN_NEIGHBOR_BEARER_SUSPECT;
            }
            if (bearer->state == UCN_NEIGHBOR_BEARER_SUSPECT &&
                ucn_elapsed_at_least(now_ms, bearer->last_seen_ms,
                                     link_remove_timeout_ms(bearer->link))) {
                bearer->state = UCN_NEIGHBOR_BEARER_DOWN;
                if (entry->primary_bearer_index == bearer_index) {
                    entry->primary_bearer_index = UCN_NEIGHBOR_PRIMARY_BEARER_NONE;
                }
            }
        }
        refresh_neighbor_liveness_state(node, entry, now_ms);
    }
}

/*
 * EN: Allocates `allocate_sequence` from fixed Lite/Full Node slots without heap use.
 * 中文：从 Lite/Full Node 的固定槽位分配 `allocate_sequence`，不使用堆内存。
 */
static ucn_result_t allocate_sequence(ucn_node_t *node, ucn_sequence_t *sequence)
{
    ucn_sequence_t next_sequence;
    ucn_result_t result;

    if (node->next_sequence == 0U) {
        return UCN_ERR_SECURITY;
    }
    if (node->next_sequence >= UCN_SEQUENCE_ROTATION_THRESHOLD) {
        ucn_session_id_t new_session_id = 0U;
        ucn_sequence_t rotated_sequence = 0U;

        if (node->security_ops == NULL ||
            node->security_ops->rotate_session == NULL) {
            return UCN_ERR_SECURITY;
        }
        result = node->security_ops->rotate_session(node->security_context,
                                                     node->session_id,
                                                     &new_session_id,
                                                     &rotated_sequence);
        if (result != UCN_OK) {
            return result;
        }
        if (new_session_id == 0U || new_session_id == node->session_id ||
            new_session_id > ucn_wire_profile_get_descriptor(
                                 node->tx_wire_profile)->max_wire_value ||
            rotated_sequence == 0U ||
            rotated_sequence >= UCN_SEQUENCE_ROTATION_THRESHOLD) {
            return UCN_ERR_SECURITY;
        }
        node->session_id = new_session_id;
        node->next_sequence = rotated_sequence;
        node->stats.session_rotations++;
    }

    *sequence = node->next_sequence;
    next_sequence = node->next_sequence + 1U;
    if (node->security_ops != NULL) {
        result = node->security_ops->store_next_sequence(node->security_context,
                                                         next_sequence);
        if (result != UCN_OK) {
            return result;
        }
    }
    node->next_sequence = next_sequence;
    return UCN_OK;
}

/*
 * EN: Copies or submits `queue_items` to a bounded Lite/Full Node queue.
 * 中文：把 `queue_items` 复制或提交到固定容量的 Lite/Full Node 队列。
 */
static ucn_tx_item_t *queue_items(ucn_node_t *node,
                                  ucn_traffic_class_t traffic_class,
                                  size_t *count)
{
    switch (traffic_class) {
    case UCN_TRAFFIC_Q0_CRITICAL:
        *count = UCN_TX_Q0_DEPTH;
        return node->q0;
    case UCN_TRAFFIC_Q1_REALTIME:
        *count = UCN_TX_Q1_DEPTH;
        return node->q1;
    case UCN_TRAFFIC_Q2_NORMAL:
        *count = UCN_TX_Q2_DEPTH;
        return node->q2;
    case UCN_TRAFFIC_Q3_BULK:
        *count = UCN_TX_Q3_DEPTH;
        return node->q3;
    default:
        *count = 0U;
        return NULL;
    }
}

/*
 * EN: Searches bounded Lite/Full Node state for `next_item`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `next_item`。
 */
static ucn_tx_item_t *find_next_item(ucn_tx_item_t *items, size_t count)
{
    size_t index;
    ucn_tx_item_t *oldest = NULL;

    for (index = 0U; index < count; ++index) {
        if (items[index].occupied &&
            (oldest == NULL || (int32_t)(items[index].order - oldest->order) < 0)) {
            oldest = &items[index];
        }
    }

    return oldest;
}

/* Interleaved 6:3:2:1 weighted service.  Q0 appears at least every second
 * slot, while a continuously backlogged Q3 is still served once per cycle. */
#define UCN_BUSINESS_SCHEDULE_LENGTH ((uint8_t)12U)
static const ucn_traffic_class_t ucn_business_schedule[UCN_BUSINESS_SCHEDULE_LENGTH] = {
    UCN_TRAFFIC_Q0_CRITICAL,
    UCN_TRAFFIC_Q1_REALTIME,
    UCN_TRAFFIC_Q0_CRITICAL,
    UCN_TRAFFIC_Q2_NORMAL,
    UCN_TRAFFIC_Q0_CRITICAL,
    UCN_TRAFFIC_Q1_REALTIME,
    UCN_TRAFFIC_Q0_CRITICAL,
    UCN_TRAFFIC_Q3_BULK,
    UCN_TRAFFIC_Q0_CRITICAL,
    UCN_TRAFFIC_Q1_REALTIME,
    UCN_TRAFFIC_Q0_CRITICAL,
    UCN_TRAFFIC_Q2_NORMAL
};

/*
 * EN: Selects the next queued business item using bounded weighted service.
 * 中文：使用有界加权服务选择下一条排队业务消息。
 */
static ucn_tx_item_t *select_business_item(ucn_node_t *node,
                                           uint8_t *next_cursor)
{
    uint8_t offset;
    ucn_tx_item_t *retained_q0;

    retained_q0 = find_next_item(node->q0, UCN_TX_Q0_DEPTH);
    if (retained_q0 != NULL &&
        retained_q0->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE &&
        (retained_q0->next_attempt_ms != 0U ||
         retained_q0->backpressure_retries != 0U
#if UCN_FEATURE_DYNAMIC_MESH
         || retained_q0->waiting_for_route
#endif
        )) {
        *next_cursor = node->business_schedule_cursor;
        return retained_q0;
    }

    for (offset = 0U; offset < UCN_BUSINESS_SCHEDULE_LENGTH; ++offset) {
        const uint8_t schedule_index = (uint8_t)(
            (node->business_schedule_cursor + offset) % UCN_BUSINESS_SCHEDULE_LENGTH);
        const ucn_traffic_class_t traffic_class =
            ucn_business_schedule[schedule_index];
        size_t count;
        ucn_tx_item_t *items = queue_items(node, traffic_class, &count);
        ucn_tx_item_t *item = find_next_item(items, count);

        if (item != NULL) {
            *next_cursor = (uint8_t)(
                (schedule_index + 1U) % UCN_BUSINESS_SCHEDULE_LENGTH);
            return item;
        }
    }
    *next_cursor = node->business_schedule_cursor;
    return NULL;
}

/*
 * EN: Checks the `queue_pending_q1` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `queue_pending_q1` 条件。
 */
static ucn_result_t queue_pending_q1(ucn_node_t *node,
                                     ucn_node_id_t destination,
                                     uint8_t message_type,
                                     const uint8_t *payload,
                                     uint16_t payload_length)
{
    ucn_pending_q1_item_t *slot = NULL;
    bool overwrote_latest = false;
    size_t index;

    if (payload_length > UCN_MAX_PAYLOAD_BYTES) {
        return UCN_ERR_TOO_LARGE;
    }
    for (index = 0U; index < UCN_PENDING_Q1_DEPTH; ++index) {
        if (node->pending_q1[index].occupied &&
            node->pending_q1[index].destination == destination &&
            node->pending_q1[index].message_type == message_type) {
            slot = &node->pending_q1[index];
            overwrote_latest = true;
            break;
        }
        if (!node->pending_q1[index].occupied && slot == NULL) {
            slot = &node->pending_q1[index];
        }
    }
    if (slot == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    slot->occupied = true;
    slot->destination = destination;
    slot->message_type = message_type;
    slot->deadline_ms =
        ucn_deadline_from_now(node->now_ms, UCN_PENDING_Q1_TIMEOUT_MS);
    slot->payload_length = payload_length;
    if (payload_length != 0U) {
        (void)memcpy(slot->payload, payload, payload_length);
    }
    node->stats.q1_route_wait_queued++;
    if (overwrote_latest) {
        node->stats.q1_route_wait_latest_overwritten++;
    }
    return UCN_OK;
}

/*
 * EN: Validates and submits `send_pending_q1_if_ready` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_pending_q1_if_ready` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_pending_q1_if_ready(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_PENDING_Q1_DEPTH; ++index) {
        ucn_pending_q1_item_t *item = &node->pending_q1[index];

        if (!item->occupied) {
            continue;
        }
        if (ucn_deadline_expired(now_ms, item->deadline_ms)) {
            item->occupied = false;
            node->stats.q1_route_wait_expired++;
            return UCN_ERR_TTL;
        }
        if (find_link(node, item->destination) != NULL) {
            ucn_result_t result;

            result = ucn_endpoint_is_static((ucn_endpoint_t)item->message_type) ?
                         send_endpoint_internal(
                             node, item->destination,
                             (ucn_endpoint_t)item->message_type,
                             UCN_TRAFFIC_Q1_REALTIME, item->payload,
                             item->payload_length, false) :
                         ucn_node_send(node, item->destination,
                                       item->message_type,
                                       UCN_TRAFFIC_Q1_REALTIME, item->payload,
                                       item->payload_length);
            if (result == UCN_OK) {
                item->occupied = false;
                node->stats.q1_route_wait_sent++;
                return UCN_OK;
            }
            if (result == UCN_ERR_NOT_FOUND || result == UCN_ERR_NO_SPACE ||
                result == UCN_ERR_LINK_DOWN) {
                node->stats.q1_route_wait_retried++;
                return result;
            }
            item->occupied = false;
            node->stats.q1_route_wait_permanent_failed++;
            return result;
        }
    }
    return UCN_ERR_NOT_FOUND;
}

/*
 * EN: Returns the current `link_status` view from Lite/Full Node state.
 * 中文：从 Lite/Full Node 状态返回当前 `link_status` 视图。
 */
static ucn_result_t get_link_status(const ucn_link_t *link, ucn_link_status_t *status)
{
    if (link == NULL || link->ops == NULL || link->ops->send == NULL ||
        link->ops->get_status == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    return link->ops->get_status(link, status);
}

/*
 * EN: Checks the `link_is_usable` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `link_is_usable` 条件。
 */
static bool link_is_usable(const ucn_link_t *link)
{
    ucn_link_status_t status;

    return get_link_status(link, &status) == UCN_OK && status.is_up &&
           ucn_link_effective_mtu(link, &status) != 0U;
}

/*
 * EN: Calculates `link_heartbeat_interval_ms` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `link_heartbeat_interval_ms`。
 */
static uint32_t link_heartbeat_interval_ms(const ucn_link_t *link)
{
    return link != NULL &&
                   link->liveness_profile == (uint8_t)UCN_LINK_LIVENESS_FAST ?
               UCN_LINK_LIVENESS_FAST_HEARTBEAT_INTERVAL_MS :
               UCN_HEARTBEAT_INTERVAL_MS;
}

/*
 * EN: Derives a directional, deterministic post-admission Heartbeat phase so
 * MCU nodes do not keep every Bearer synchronized onto one radio burst. The
 * phase is local state only and does not change the wire format.
 * 中文：为入网后的心跳计算带方向且确定性的相位，避免 MCU 把所有 Bearer
 * 长期同步成同一无线突发；该相位只属于本地状态，不改变线协议格式。
 */
static uint32_t initial_heartbeat_phase_ms(const ucn_node_t *node,
                                           const ucn_neighbor_entry_t *entry,
                                           const ucn_neighbor_bearer_t *bearer)
{
    const uint32_t interval_ms = link_heartbeat_interval_ms(bearer->link);
    uint32_t mixed = (uint32_t)node->config.node_id * UINT32_C(0x9E3779B1);

    mixed ^= (uint32_t)entry->peer_node_id * UINT32_C(0x85EBCA6B);
    mixed ^= (uint32_t)bearer->link->link_id * UINT32_C(0xC2B2AE35);
    mixed ^= mixed >> 16U;
    mixed *= UINT32_C(0x7FEB352D);
    mixed ^= mixed >> 15U;
    mixed *= UINT32_C(0x846CA68B);
    mixed ^= mixed >> 16U;
    return mixed % interval_ms;
}

/*
 * EN: Calculates `link_suspect_timeout_ms` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `link_suspect_timeout_ms`。
 */
static uint32_t link_suspect_timeout_ms(const ucn_link_t *link)
{
    return link != NULL &&
                   link->liveness_profile == (uint8_t)UCN_LINK_LIVENESS_FAST ?
               UCN_LINK_LIVENESS_FAST_SUSPECT_TIMEOUT_MS :
               UCN_NEIGHBOR_SUSPECT_TIMEOUT_MS;
}

/*
 * EN: Calculates `link_remove_timeout_ms` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `link_remove_timeout_ms`。
 */
static uint32_t link_remove_timeout_ms(const ucn_link_t *link)
{
    return link != NULL &&
                   link->liveness_profile == (uint8_t)UCN_LINK_LIVENESS_FAST ?
               UCN_LINK_LIVENESS_FAST_REMOVE_TIMEOUT_MS :
               UCN_NEIGHBOR_REMOVE_TIMEOUT_MS;
}

/*
 * EN: Selects or resolves `resolve_link_local_receive_profile` using deterministic Lite/Full Node rules.
 * 中文：按照确定性的 Lite/Full Node 规则选择或解析 `resolve_link_local_receive_profile`。
 */
static ucn_result_t resolve_link_local_receive_profile(
    const ucn_node_t *node,
    const ucn_link_t *link,
    ucn_wire_profile_t *profile)
{
    ucn_wire_profile_t configured;

    if (node == NULL || link == NULL || profile == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    configured = (ucn_wire_profile_t)link->local_receive_wire_profile;
    if (configured == UCN_WIRE_PROFILE_UNSPECIFIED) {
        *profile = node->max_receive_wire_profile;
        return UCN_OK;
    }
    if (ucn_wire_profile_get_descriptor(configured) == NULL ||
        configured > node->max_receive_wire_profile) {
        return UCN_ERR_CONFIG;
    }
    *profile = configured;
    return UCN_OK;
}

/*
 * EN: Builds `prepare_outbound_wire_profile` in caller-provided storage for Lite/Full Node.
 * 中文：在调用方存储中为 Lite/Full Node 构造 `prepare_outbound_wire_profile`。
 */
static ucn_result_t prepare_outbound_wire_profile(
    const ucn_node_t *node,
    const ucn_link_t *link,
    const ucn_link_status_t *status,
    ucn_wire_profile_t additional_maximum_profile,
    size_t additional_minimum_mtu,
    ucn_frame_t *frame)
{
    ucn_wire_profile_t link_maximum_profile = UCN_WIRE_PROFILE_W3_BACKBONE;
    ucn_wire_profile_t originated_maximum_profile;
    size_t maximum_frame_bytes = ucn_link_effective_mtu(link, status);

    if (link->peer_wire_profile != UCN_WIRE_PROFILE_UNSPECIFIED) {
        if (ucn_wire_profile_get_descriptor(link->peer_wire_profile) == NULL) {
            return UCN_ERR_CONFIG;
        }
        if (link_maximum_profile > link->peer_wire_profile) {
            link_maximum_profile = link->peer_wire_profile;
        }
    }
    if (additional_maximum_profile != UCN_WIRE_PROFILE_UNSPECIFIED) {
        if (ucn_wire_profile_get_descriptor(additional_maximum_profile) == NULL) {
            return UCN_ERR_CONFIG;
        }
        if (additional_maximum_profile < link_maximum_profile) {
            link_maximum_profile = additional_maximum_profile;
        }
    }
    if (additional_minimum_mtu != 0U &&
        additional_minimum_mtu < maximum_frame_bytes) {
        maximum_frame_bytes = additional_minimum_mtu;
    }
    if (maximum_frame_bytes == 0U) {
        return UCN_ERR_LINK_DOWN;
    }
    if (frame->wire_profile != UCN_WIRE_PROFILE_UNSPECIFIED) {
        const size_t encoded_size = ucn_frame_encoded_size(frame);

        if (frame->wire_profile > link_maximum_profile) {
            return UCN_ERR_UNSUPPORTED;
        }
        return encoded_size != 0U && encoded_size <= maximum_frame_bytes ?
            UCN_OK : UCN_ERR_TOO_LARGE;
    }
    originated_maximum_profile = node->tx_wire_profile < link_maximum_profile ?
        node->tx_wire_profile : link_maximum_profile;
    if (!node->automatic_wire_profile) {
        size_t encoded_size;

        if (node->tx_wire_profile > link_maximum_profile) {
            return UCN_ERR_UNSUPPORTED;
        }
        frame->wire_profile = node->tx_wire_profile;
        encoded_size = ucn_frame_encoded_size(frame);
        return encoded_size != 0U && encoded_size <= maximum_frame_bytes ?
            UCN_OK : UCN_ERR_TOO_LARGE;
    }
    return ucn_frame_select_min_wire_profile(
        frame, originated_maximum_profile, maximum_frame_bytes,
        &frame->wire_profile);
}

/*
 * EN: Validates and submits `send_frame_on_link` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_frame_on_link` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_frame_on_link(ucn_node_t *node,
                                       ucn_link_t *link,
                                       const ucn_frame_t *frame)
{
    ucn_frame_t prepared;
    ucn_link_status_t status;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    ucn_result_t result;

    if (node == NULL || link == NULL || frame == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    prepared = *frame;
    if (!ucn_node_security_ready(node)) {
        return UCN_ERR_SECURITY;
    }

    result = get_link_status(link, &status);
    if (result != UCN_OK) {
        if (result == UCN_ERR_LINK_DOWN) {
            mark_neighbor_bearer_down(node, link);
        }
        return result;
    }
    if (!status.is_up) {
        mark_neighbor_bearer_down(node, link);
        return UCN_ERR_LINK_DOWN;
    }
    if (ucn_link_effective_mtu(link, &status) == 0U) {
        mark_neighbor_bearer_down(node, link);
        return UCN_ERR_LINK_DOWN;
    }
    result = prepare_outbound_wire_profile(
        node, link, &status, UCN_WIRE_PROFILE_UNSPECIFIED, 0U, &prepared);
    if (result != UCN_OK) {
        return result;
    }
    if (node->security_ops != NULL) {
        result = node->security_ops->authorize_tx(node->security_context,
                                                  &prepared);
        if (result != UCN_OK) {
            return result;
        }
    }

    result = ucn_frame_encode(&prepared, encoded, sizeof(encoded),
                              &encoded_length);
    if (result != UCN_OK) {
        return result;
    }
    if (encoded_length > ucn_link_effective_mtu(link, &status)) {
        return UCN_ERR_TOO_LARGE;
    }

    result = link->ops->send(link, encoded, encoded_length);
    if (result == UCN_OK) {
        node->stats.tx_sent++;
        if ((uint8_t)frame->traffic_class < (uint8_t)UCN_TRAFFIC_CLASS_COUNT) {
            node->stats.tx_sent_by_class[(uint8_t)frame->traffic_class]++;
        }
    } else {
        node->stats.tx_error_dropped++;
        if (result == UCN_ERR_LINK_DOWN) {
            mark_neighbor_bearer_down(node, link);
        }
    }
    return result;
}

/* A Driver may report LINK_DOWN only after its status snapshot was selected.
 * That first attempt cannot have delivered successfully, so a single retry on
 * the same logical Neighbor's newly selected admitted Bearer is safe. */
/*
 * EN: Validates and submits `send_frame_on_logical_egress` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_frame_on_logical_egress` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_frame_on_logical_egress(ucn_node_t *node,
                                                 ucn_link_t *configured_link,
                                                 const ucn_frame_t *frame,
                                                 ucn_link_t **last_link)
{
    ucn_link_t *link;
    ucn_link_t *first_link;
    ucn_result_t result;

    if (last_link != NULL) {
        *last_link = NULL;
    }
    if (node == NULL || configured_link == NULL || frame == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    link = resolve_egress_link(node, configured_link);
    if (link == NULL) {
        return UCN_ERR_LINK_DOWN;
    }
    first_link = link;
    if (last_link != NULL) {
        *last_link = link;
    }
    result = send_frame_on_link(node, link, frame);
    if (result != UCN_ERR_LINK_DOWN) {
        return result;
    }

    configured_link = link;
    link = resolve_egress_link(node, configured_link);
    if (link == NULL || link == first_link) {
        return UCN_ERR_LINK_DOWN;
    }
    if (last_link != NULL) {
        *last_link = link;
    }
    return send_frame_on_link(node, link, frame);
}

/*
 * EN: Validates `protect_outbound_business` against the Lite/Full Node security or authorization contract.
 * 中文：按照 Lite/Full Node 的安全或授权合同验证 `protect_outbound_business`。
 */
static ucn_result_t protect_outbound_business(ucn_node_t *node,
                                              ucn_link_t *link,
                                              const ucn_link_status_t *status,
                                              ucn_wire_profile_t maximum_profile,
                                              size_t minimum_mtu,
                                              ucn_frame_t *frame,
                                              uint8_t *ciphertext,
                                              uint8_t auth_tag[UCN_E2E_TAG_SIZE])
{
    const ucn_security_policy_t *policy;
    bool protected_frame = false;
    ucn_result_t result;

    if (ucn_message_type_is_control(frame->message_type)) {
        return (frame->flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U ?
               UCN_ERR_MALFORMED : UCN_OK;
    }
    policy = resolve_security_policy(node, frame->message_type);
    if (policy->tx_mode == UCN_SECURITY_TX_E2E_PROTECTED) {
        protected_frame = true;
    } else if (policy->tx_mode == UCN_SECURITY_TX_AUTO &&
               node->security_ops != NULL &&
               node->security_ops->select_tx_protection != NULL) {
        result = node->security_ops->select_tx_protection(node->security_context,
                                                          frame, &protected_frame);
        if (result != UCN_OK) {
            return result;
        }
    }
    if (protected_frame) {
        if (node->security_ops == NULL || node->security_ops->seal == NULL ||
            node->security_ops->open == NULL) {
            return UCN_ERR_SECURITY;
        }
        frame->flags |= UCN_FRAME_FLAG_E2E_PROTECTED;
        /* Size/profile validation includes the authentication tag.  Point at
         * the caller-owned output buffer before validation; seal() fills it
         * only after the final Wire Class has been selected. */
        frame->auth_tag = auth_tag;
    }
    result = prepare_outbound_wire_profile(node, link, status, maximum_profile,
                                           minimum_mtu, frame);
    if (result != UCN_OK || !protected_frame) {
        return result;
    }
    result = node->security_ops->seal(node->security_context, frame, frame->payload,
                                      frame->payload_length, ciphertext, auth_tag);
    if (result != UCN_OK) {
        return result;
    }
    frame->payload = ciphertext;
    return UCN_OK;
}

/*
 * EN: Validates `inbound_business_security` before Lite/Full Node state is used or changed.
 * 中文：在使用或修改 Lite/Full Node 状态前验证 `inbound_business_security`。
 */
static ucn_result_t validate_inbound_business_security(
    ucn_node_t *node,
    ucn_link_t *ingress_link,
    ucn_frame_t *frame,
    uint8_t *plaintext)
{
    const ucn_security_policy_t *policy;
    bool protected_frame = (frame->flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U;

    if (ucn_message_type_is_control(frame->message_type)) {
        return protected_frame ? UCN_ERR_MALFORMED : UCN_OK;
    }
    if (frame->destination == UCN_NODE_BROADCAST) {
        return protected_frame ? UCN_ERR_UNSUPPORTED : UCN_OK;
    }

    policy = resolve_security_policy(node, frame->message_type);
    if (frame->destination != node->config.node_id) {
        if (policy->forward_mode == UCN_SECURITY_FORWARD_TERMINAL_ONLY ||
            (policy->forward_mode == UCN_SECURITY_FORWARD_OPAQUE_E2E_ONLY &&
             !protected_frame)) {
            return UCN_ERR_ACCESS;
        }
        return UCN_OK;
    }

    if ((policy->rx_mode == UCN_SECURITY_RX_PLAIN_ONLY && protected_frame) ||
        (policy->rx_mode == UCN_SECURITY_RX_ENCRYPTED_ONLY && !protected_frame)) {
        return UCN_ERR_ACCESS;
    }
    if (!protected_frame) {
        return UCN_OK;
    }
    if (node->security_ops == NULL || node->security_ops->open == NULL) {
        return UCN_ERR_SECURITY;
    }
    if (frame->auth_tag == NULL) {
        return UCN_ERR_MALFORMED;
    }
    {
        ucn_result_t result = node->security_ops->open(node->security_context,
                                                        ingress_link, frame,
                                                        frame->payload,
                                                        frame->payload_length,
                                                        frame->auth_tag,
                                                        plaintext);
        if (result != UCN_OK) {
            return result;
        }
    }
    frame->payload = plaintext;
    return UCN_OK;
}

/*
 * EN: Validates and submits `send_control_on_link_profile` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_control_on_link_profile` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_control_on_link_profile(
    ucn_node_t *node,
    ucn_link_t *link,
    ucn_node_id_t destination,
    uint8_t message_type,
    const uint8_t *payload,
    uint16_t payload_length,
    ucn_wire_profile_t wire_profile)
{
    ucn_frame_t frame;
    const ucn_route_entry_t *route;
    ucn_result_t result;

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = message_type;
    frame.wire_profile = wire_profile;
    frame.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    route = find_active_route(node, destination);
    frame.hop_limit = (message_type == UCN_MSG_HELLO ||
                       message_type == UCN_MSG_HEARTBEAT ||
                       link->peer_node_id == destination) ? 1U :
                      (route != NULL && !route->is_static &&
                       route->hop_count != 0U ?
                           route->hop_count : node->config.default_hop_limit);
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = destination;
    frame.session_id = node->session_id;
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        return result;
    }
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = payload_length;
    result = send_frame_on_link(node, link, &frame);
    if (result == UCN_OK) {
        mark_route_used(node, destination);
    }
    return result;
}

/* Only use this wrapper for control payloads whose semantics do not contain
 * additional profile-sized identifiers.  Profile-dependent payload codecs are
 * migrated explicitly in V5-14/V5-15 instead of being silently narrowed. */
/*
 * EN: Validates and submits `send_adaptive_control_on_link` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_adaptive_control_on_link` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_adaptive_control_on_link(
    ucn_node_t *node,
    ucn_link_t *link,
    ucn_node_id_t destination,
    uint8_t message_type,
    const uint8_t *payload,
    uint16_t payload_length)
{
    const ucn_wire_profile_t profile = node->automatic_wire_profile ?
        UCN_WIRE_PROFILE_UNSPECIFIED : node->tx_wire_profile;

    return send_control_on_link_profile(node, link, destination, message_type,
                                        payload, payload_length, profile);
}

#if UCN_FEATURE_PATH
/*
 * EN: Selects or resolves `select_path_control_profile` using deterministic Lite/Full Node rules.
 * 中文：按照确定性的 Lite/Full Node 规则选择或解析 `select_path_control_profile`。
 */
static ucn_result_t select_path_control_profile(
    const ucn_node_t *node,
    const ucn_link_t *link,
    ucn_node_id_t control_target,
    uint8_t message_type,
    ucn_path_id_t path_id,
    ucn_node_id_t destination,
    ucn_node_id_t next_hop,
    bool capable_path_install,
    ucn_wire_profile_t *selected_profile)
{
    ucn_link_status_t status;
    ucn_wire_profile_t first_profile;
    ucn_wire_profile_t profile;
    ucn_result_t result;

    if (node == NULL || link == NULL || selected_profile == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = get_link_status(link, &status);
    if (result != UCN_OK) {
        return result;
    }
    first_profile = node->automatic_wire_profile ?
        UCN_WIRE_PROFILE_W0_LOCAL : node->tx_wire_profile;
    for (profile = first_profile; profile <= node->tx_wire_profile; ++profile) {
        const ucn_wire_profile_descriptor_t *descriptor =
            ucn_wire_profile_get_descriptor(profile);
        uint8_t placeholder[UCN_PATH_INSTALL_MAX_PAYLOAD_BYTES] = { 0U };
        ucn_frame_t frame;
        size_t payload_length;
        size_t encoded_size;
        size_t path_maximum;

        if (descriptor == NULL ||
            (link->peer_wire_profile != UCN_WIRE_PROFILE_UNSPECIFIED &&
             profile > link->peer_wire_profile)) {
            continue;
        }
        path_maximum = maximum_value_for_wire_bytes(descriptor->path_id_bytes);
        if (path_id > path_maximum || destination > descriptor->max_node_id ||
            next_hop > descriptor->max_node_id) {
            continue;
        }
        if (message_type == UCN_MSG_PATH_INSTALL) {
            payload_length = capable_path_install ?
                path_install_capable_payload_size(profile) :
                path_install_base_payload_size(profile);
        } else {
            payload_length = path_revoke_payload_size(profile);
        }
        (void)memset(&frame, 0, sizeof(frame));
        frame.message_type = message_type;
        frame.wire_profile = profile;
        frame.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
        frame.hop_limit = node->config.default_hop_limit;
        frame.network_id = node->config.network_id;
        frame.source = node->config.node_id;
        frame.destination = control_target;
        frame.sequence = 1U;
        frame.session_id = node->session_id;
        frame.payload = placeholder;
        frame.payload_length = (uint16_t)payload_length;
        encoded_size = ucn_frame_encoded_size(&frame);
        if (encoded_size != 0U &&
            encoded_size <= ucn_link_effective_mtu(link, &status)) {
            *selected_profile = profile;
            return UCN_OK;
        }
    }
    return UCN_ERR_TOO_LARGE;
}

/*
 * EN: Validates and submits `send_control_to_node_profile` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_control_to_node_profile` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_control_to_node_profile(
    ucn_node_t *node,
    ucn_node_id_t control_target,
    uint8_t message_type,
    const uint8_t *payload,
    uint16_t payload_length,
    ucn_wire_profile_t wire_profile)
{
    ucn_link_t *link;

    if (node == NULL || control_target == 0U ||
        control_target == UCN_NODE_BROADCAST) {
        return UCN_ERR_ARGUMENT;
    }
    link = find_link(node, control_target);
    if (link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    return send_control_on_link_profile(node, link, control_target,
                                        message_type, payload, payload_length,
                                        wire_profile);
}
#endif

#if UCN_FEATURE_PATH
/*
 * EN: Creates the wrap-safe expiration deadline for a Path lease.
 * 中文：为 Path 租约生成回绕安全的到期时间。
 */
static uint32_t path_expires_at(const ucn_node_t *node, uint32_t lease_ms)
{
    return node == NULL ? 0U : ucn_deadline_from_now(node->now_ms, lease_ms);
}

/*
 * EN: Validates and installs `install_path_forward_entry` into bounded Lite/Full Node state.
 * 中文：验证 `install_path_forward_entry` 并将其安装到固定容量的 Lite/Full Node 状态中。
 */
static ucn_result_t install_path_forward_entry(ucn_node_t *node,
                                               ucn_node_id_t owner,
                                               ucn_session_id_t owner_session_id,
                                               ucn_path_id_t path_id,
                                               ucn_node_id_t destination,
                                               ucn_node_id_t next_hop,
                                               uint8_t remaining_hops,
                                               uint32_t lease_ms,
                                               const ucn_path_capability_t *requested_capability)
{
    ucn_path_forward_config_t config;
    const ucn_path_capability_t *capability_to_install = NULL;
    ucn_path_capability_t installed_capability;

    if (node == NULL || owner == 0U || owner == UCN_NODE_BROADCAST ||
        owner_session_id == 0U || path_id == 0U || destination == 0U ||
        destination == UCN_NODE_BROADCAST ||
        remaining_hops > UCN_MAX_HOPS ||
        (next_hop == 0U && remaining_hops != 0U) ||
        (next_hop != 0U && remaining_hops == 0U) ||
        !ucn_duration_is_valid(lease_ms)) {
        return UCN_ERR_ARGUMENT;
    }

    (void)memset(&config, 0, sizeof(config));
    config.owner = owner;
    config.owner_session_id = owner_session_id;
    config.path_id = path_id;
    config.destination = destination;
    config.next_hop = next_hop;
    config.remaining_hops = remaining_hops;
    config.expires_at_ms = path_expires_at(node, lease_ms);
    if (next_hop == 0U) {
        if (destination != node->config.node_id) {
            return UCN_ERR_ARGUMENT;
        }
    } else {
        ucn_path_effective_capability_t capability;
        ucn_result_t result;

        if (destination == node->config.node_id || next_hop == node->config.node_id) {
            return UCN_ERR_ARGUMENT;
        }
        config.egress_link = find_direct_link(node, next_hop);
        if (config.egress_link == NULL) {
            return UCN_ERR_NOT_FOUND;
        }
        result = resolve_path_bearer_capability(node, config.egress_link,
                                                &capability);
        if (result != UCN_OK) {
            return result;
        }
        if (requested_capability != NULL) {
            if (requested_capability->minimum_mtu == 0U ||
                ucn_wire_profile_get_descriptor(
                    requested_capability->maximum_wire_profile) == NULL) {
                return UCN_ERR_ARGUMENT;
            }
            if (requested_capability->maximum_wire_profile <
                capability.maximum_wire_profile) {
                capability.maximum_wire_profile =
                    requested_capability->maximum_wire_profile;
            }
            if ((size_t)requested_capability->minimum_mtu <
                capability.minimum_mtu) {
                capability.minimum_mtu = requested_capability->minimum_mtu;
            }
        }
        installed_capability.maximum_wire_profile =
            capability.maximum_wire_profile;
        installed_capability.minimum_mtu = (uint16_t)capability.minimum_mtu;
        capability_to_install = &installed_capability;
    }
    return ucn_path_install_capable(&node->path_state, &config,
                                    capability_to_install);
}
#endif

#if UCN_FEATURE_DIAGNOSTICS
/*
 * EN: Selects or resolves `select_path_trace_profile` using deterministic Lite/Full Node rules.
 * 中文：按照确定性的 Lite/Full Node 规则选择或解析 `select_path_trace_profile`。
 */
static ucn_result_t select_path_trace_profile(
    const ucn_node_t *node,
    const ucn_link_t *link,
    ucn_node_id_t destination,
    uint8_t record_count,
    ucn_wire_profile_t *selected_profile)
{
    ucn_link_status_t status;
    ucn_wire_profile_t first_profile;
    ucn_wire_profile_t profile;
    ucn_result_t result;

    if (node == NULL || link == NULL || selected_profile == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = get_link_status(link, &status);
    if (result != UCN_OK) {
        return result;
    }
    first_profile = node->automatic_wire_profile ?
        UCN_WIRE_PROFILE_W0_LOCAL : node->tx_wire_profile;
    for (profile = first_profile; profile <= node->tx_wire_profile; ++profile) {
        uint8_t placeholder[UCN_PATH_TRACE_MAX_PAYLOAD_BYTES] = { 0U };
        ucn_frame_t frame;
        size_t encoded_size;

        if (link->peer_wire_profile != UCN_WIRE_PROFILE_UNSPECIFIED &&
            profile > link->peer_wire_profile) {
            continue;
        }
        (void)memset(&frame, 0, sizeof(frame));
        frame.message_type = UCN_MSG_PATH_TRACE_REQ;
        frame.wire_profile = profile;
        frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
        frame.flags = UCN_FRAME_FLAG_DIAGNOSTIC;
        frame.hop_limit = node->config.default_hop_limit;
        frame.network_id = node->config.network_id;
        frame.source = node->config.node_id;
        frame.destination = destination;
        frame.sequence = 1U;
        frame.session_id = node->session_id;
        frame.payload = placeholder;
        frame.payload_length = (uint16_t)path_trace_payload_size(
            profile, record_count);
        encoded_size = ucn_frame_encoded_size(&frame);
        if (encoded_size != 0U &&
            encoded_size <= ucn_link_effective_mtu(link, &status)) {
            *selected_profile = profile;
            return UCN_OK;
        }
    }
    return UCN_ERR_TOO_LARGE;
}

/*
 * EN: Checks the `path_trace_status_is_wire_valid` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `path_trace_status_is_wire_valid` 条件。
 */
static bool path_trace_status_is_wire_valid(uint8_t status)
{
    return status <= (uint8_t)UCN_PATH_TRACE_STATUS_TRUNCATED;
}

/*
 * EN: Checks whether `path_trace_payload` satisfies the Lite/Full Node module's validity rules.
 * 中文：检查 `path_trace_payload` 是否满足 Lite/Full Node 模块的合法性规则。
 */
static bool path_trace_payload_is_valid(const ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor;
    uint8_t record_count;
    uint8_t record_limit;
    size_t expected_length;
    size_t index;

    if (frame == NULL || frame->payload == NULL ||
        frame->payload_length < UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES ||
        (frame->flags & UCN_FRAME_FLAG_DIAGNOSTIC) == 0U ||
        frame->has_route_extension ||
        read_u32_be(frame->payload + UCN_PATH_TRACE_TRACE_ID_OFFSET) == 0U) {
        return false;
    }
    descriptor = ucn_wire_profile_get_descriptor(frame->wire_profile);
    if (descriptor == NULL) {
        return false;
    }
    record_count = frame->payload[UCN_PATH_TRACE_RECORD_COUNT_OFFSET];
    record_limit = frame->payload[UCN_PATH_TRACE_RECORD_LIMIT_OFFSET];
    expected_length = path_trace_payload_size(frame->wire_profile, record_count);
    if (record_count == 0U || record_limit == 0U ||
        record_count > record_limit ||
        record_count > UCN_PATH_TRACE_MAX_NODES ||
        record_limit > UCN_PATH_TRACE_MAX_NODES ||
        frame->payload[UCN_PATH_TRACE_RESERVED_OFFSET] != 0U ||
        !path_trace_status_is_wire_valid(frame->payload[UCN_PATH_TRACE_STATUS_OFFSET]) ||
        frame->payload_length != expected_length) {
        return false;
    }
    for (index = 0U; index < record_count; ++index) {
        ucn_node_id_t node_id = read_uint_be(
            frame->payload + UCN_PATH_TRACE_NODE_IDS_OFFSET +
                index * descriptor->address_bytes,
            descriptor->address_bytes);

        if (node_id == 0U || node_id == UCN_NODE_BROADCAST) {
            return false;
        }
    }
    return true;
}

/*
 * EN: Appends `append_path_trace_node` to bounded Lite/Full Node storage.
 * 中文：把 `append_path_trace_node` 追加到固定容量的 Lite/Full Node 存储中。
 */
static bool append_path_trace_node(const ucn_frame_t *frame,
                                   ucn_node_id_t node_id,
                                   uint8_t *output,
                                   uint16_t *output_length)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        frame == NULL ? NULL :
        ucn_wire_profile_get_descriptor(frame->wire_profile);
    uint8_t record_count;
    uint8_t record_limit;
    size_t node_offset;

    if (descriptor == NULL || !path_trace_payload_is_valid(frame) ||
        node_id == 0U || node_id > descriptor->max_node_id ||
        node_id == UCN_NODE_BROADCAST || output == NULL || output_length == NULL) {
        return false;
    }
    (void)memcpy(output, frame->payload, frame->payload_length);
    record_count = output[UCN_PATH_TRACE_RECORD_COUNT_OFFSET];
    record_limit = output[UCN_PATH_TRACE_RECORD_LIMIT_OFFSET];
    *output_length = frame->payload_length;
    if (record_count >= record_limit || record_count >= UCN_PATH_TRACE_MAX_NODES) {
        return false;
    }
    node_offset = UCN_PATH_TRACE_NODE_IDS_OFFSET +
                  (size_t)record_count * descriptor->address_bytes;
    write_uint_be(output + node_offset, descriptor->address_bytes, node_id);
    output[UCN_PATH_TRACE_RECORD_COUNT_OFFSET] = (uint8_t)(record_count + 1U);
    *output_length = (uint16_t)(frame->payload_length +
                                descriptor->address_bytes);
    return true;
}

/*
 * EN: Validates and submits `send_path_trace_request_on_link` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_path_trace_request_on_link` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_path_trace_request_on_link(ucn_node_t *node,
                                                     ucn_link_t *link,
                                                     ucn_node_id_t destination,
                                                     const uint8_t *payload,
                                                     uint16_t payload_length,
                                                     ucn_wire_profile_t wire_profile)
{
    ucn_frame_t frame;
    ucn_result_t result;

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_MSG_PATH_TRACE_REQ;
    frame.wire_profile = wire_profile;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.flags = UCN_FRAME_FLAG_DIAGNOSTIC;
    frame.hop_limit = node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = destination;
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = payload_length;
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        return result;
    }
    frame.session_id = node->session_id;
    return send_frame_on_link(node, link, &frame);
}

/*
 * EN: Validates and submits `send_path_trace_reply_on_link` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_path_trace_reply_on_link` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_path_trace_reply_on_link(ucn_node_t *node,
                                                   ucn_link_t *link,
                                                   ucn_node_id_t destination,
                                                   const uint8_t *payload,
                                                   uint16_t payload_length,
                                                   ucn_wire_profile_t wire_profile)
{
    ucn_frame_t frame;
    uint8_t reverse_hops;
    ucn_result_t result;

    if (payload == NULL ||
        payload_length < UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES) {
        return UCN_ERR_ARGUMENT;
    }

    reverse_hops = payload[UCN_PATH_TRACE_RECORD_COUNT_OFFSET] > 1U ?
        (uint8_t)(payload[UCN_PATH_TRACE_RECORD_COUNT_OFFSET] - 1U) : 1U;
    if (reverse_hops > node->config.default_hop_limit) {
        node->stats.hop_scope_rejected++;
        return UCN_ERR_TTL;
    }

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_MSG_PATH_TRACE_REPLY;
    frame.wire_profile = wire_profile;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.flags = UCN_FRAME_FLAG_DIAGNOSTIC;
    frame.hop_limit = reverse_hops;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = destination;
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = payload_length;
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        return result;
    }
    frame.session_id = node->session_id;
    result = send_frame_on_link(node, link, &frame);
    if (result == UCN_OK) {
        node->stats.path_trace_replies_sent++;
    }
    return result;
}

/*
 * EN: Searches bounded Lite/Full Node state for `path_trace_pending`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `path_trace_pending`。
 */
static ucn_path_trace_pending_t *find_path_trace_pending(ucn_node_t *node,
                                                          uint32_t trace_id)
{
    size_t index;

    for (index = 0U; index < UCN_PATH_TRACE_PENDING_DEPTH; ++index) {
        if (node->path_trace_pending[index].occupied &&
            node->path_trace_pending[index].trace_id == trace_id) {
            return &node->path_trace_pending[index];
        }
    }
    return NULL;
}

/*
 * EN: Searches bounded Lite/Full Node state for `free_path_trace_pending`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `free_path_trace_pending`。
 */
static ucn_path_trace_pending_t *find_free_path_trace_pending(ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_PATH_TRACE_PENDING_DEPTH; ++index) {
        if (!node->path_trace_pending[index].occupied) {
            return &node->path_trace_pending[index];
        }
    }
    return NULL;
}

/*
 * EN: Searches bounded Lite/Full Node state for `path_trace_reverse`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `path_trace_reverse`。
 */
static ucn_path_trace_reverse_t *find_path_trace_reverse(ucn_node_t *node,
                                                          ucn_node_id_t origin,
                                                          uint32_t trace_id)
{
    size_t index;

    for (index = 0U; index < UCN_PATH_TRACE_REVERSE_DEPTH; ++index) {
        ucn_path_trace_reverse_t *entry = &node->path_trace_reverse[index];

        if (entry->occupied &&
            ucn_deadline_expired(node->now_ms, entry->expires_at_ms)) {
            entry->occupied = false;
        }
        if (entry->occupied && entry->origin == origin &&
            entry->trace_id == trace_id) {
            return entry;
        }
    }
    return NULL;
}

/*
 * EN: Allocates `allocate_path_trace_reverse` from fixed Lite/Full Node slots without heap use.
 * 中文：从 Lite/Full Node 的固定槽位分配 `allocate_path_trace_reverse`，不使用堆内存。
 */
static ucn_path_trace_reverse_t *allocate_path_trace_reverse(
    ucn_node_t *node,
    ucn_node_id_t origin,
    uint32_t trace_id,
    ucn_link_t *ingress_link)
{
    size_t index;
    ucn_path_trace_reverse_t *free_slot = NULL;
    ucn_path_trace_reverse_t *existing =
        find_path_trace_reverse(node, origin, trace_id);

    if (existing != NULL) {
        existing->ingress_link = ingress_link;
        existing->expires_at_ms =
            ucn_deadline_from_now(node->now_ms, UCN_PATH_TRACE_TIMEOUT_MS);
        return existing;
    }
    for (index = 0U; index < UCN_PATH_TRACE_REVERSE_DEPTH; ++index) {
        if (!node->path_trace_reverse[index].occupied) {
            free_slot = &node->path_trace_reverse[index];
            break;
        }
    }
    if (free_slot == NULL) {
        return NULL;
    }
    (void)memset(free_slot, 0, sizeof(*free_slot));
    free_slot->occupied = true;
    free_slot->origin = origin;
    free_slot->trace_id = trace_id;
    free_slot->ingress_link = ingress_link;
    free_slot->expires_at_ms =
        ucn_deadline_from_now(node->now_ms, UCN_PATH_TRACE_TIMEOUT_MS);
    return free_slot;
}

/*
 * EN: Checks or removes expired `expire_path_trace_state` state in Lite/Full Node.
 * 中文：检查或移除 Lite/Full Node 中已过期的 `expire_path_trace_state` 状态。
 */
static void expire_path_trace_state(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_PATH_TRACE_REVERSE_DEPTH; ++index) {
        if (node->path_trace_reverse[index].occupied &&
            ucn_deadline_expired(now_ms,
                                 node->path_trace_reverse[index].expires_at_ms)) {
            node->path_trace_reverse[index].occupied = false;
        }
    }
    for (index = 0U; index < UCN_PATH_TRACE_PENDING_DEPTH; ++index) {
        ucn_path_trace_pending_t *pending = &node->path_trace_pending[index];

        if (pending->occupied &&
            ucn_deadline_expired(now_ms, pending->deadline_ms)) {
            ucn_path_trace_handler_t handler = pending->handler;
            void *context = pending->context;
            ucn_path_trace_result_t result;

            (void)memset(&result, 0, sizeof(result));
            result.status = UCN_PATH_TRACE_STATUS_TIMEOUT;
            result.trace_id = pending->trace_id;
            pending->occupied = false;
            node->stats.path_trace_timeouts++;
            if (handler != NULL) {
                handler(context, &result);
            }
        }
    }
}

/*
 * EN: Completes `complete_path_trace` and records its terminal Lite/Full Node result.
 * 中文：完成 `complete_path_trace` 并记录其 Lite/Full Node 终态结果。
 */
static ucn_result_t complete_path_trace(ucn_node_t *node,
                                        const ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(frame->wire_profile);
    ucn_path_trace_pending_t *pending;
    ucn_path_trace_result_t result;
    uint32_t trace_id;
    uint8_t record_count;
    size_t index;

    if (descriptor == NULL || !path_trace_payload_is_valid(frame)) {
        node->stats.path_trace_rejected++;
        return UCN_ERR_MALFORMED;
    }
    trace_id = read_u32_be(frame->payload + UCN_PATH_TRACE_TRACE_ID_OFFSET);
    pending = find_path_trace_pending(node, trace_id);
    if (pending == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    record_count = frame->payload[UCN_PATH_TRACE_RECORD_COUNT_OFFSET];
    if (frame->payload[UCN_PATH_TRACE_STATUS_OFFSET] ==
            (uint8_t)UCN_PATH_TRACE_STATUS_OK &&
        (frame->source != pending->destination ||
         read_uint_be(frame->payload + UCN_PATH_TRACE_NODE_IDS_OFFSET +
                      ((size_t)record_count - 1U) *
                          descriptor->address_bytes,
                      descriptor->address_bytes) !=
             pending->destination)) {
        node->stats.path_trace_rejected++;
        return UCN_ERR_MALFORMED;
    }

    (void)memset(&result, 0, sizeof(result));
    result.status = (ucn_path_trace_status_t)
        frame->payload[UCN_PATH_TRACE_STATUS_OFFSET];
    result.trace_id = trace_id;
    result.node_count = record_count;
    for (index = 0U; index < record_count; ++index) {
        result.node_ids[index] = read_uint_be(
            frame->payload + UCN_PATH_TRACE_NODE_IDS_OFFSET +
                index * descriptor->address_bytes,
            descriptor->address_bytes);
    }
    {
        ucn_path_trace_handler_t handler = pending->handler;
        void *context = pending->context;

        pending->occupied = false;
        node->stats.path_trace_completed++;
        if (handler != NULL) {
            handler(context, &result);
        }
    }
    return UCN_OK;
}

/*
 * EN: Validates and processes `handle_path_trace_request` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_path_trace_request`。
 */
static ucn_result_t handle_path_trace_request(ucn_node_t *node,
                                              ucn_link_t *ingress_link,
                                              const ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(frame->wire_profile);
    uint8_t payload[UCN_PATH_TRACE_MAX_PAYLOAD_BYTES];
    uint16_t payload_length;
    uint32_t trace_id;
    ucn_link_t *egress_link;
    ucn_result_t result;

    if (descriptor == NULL || !path_trace_payload_is_valid(frame) ||
        frame->payload[UCN_PATH_TRACE_STATUS_OFFSET] !=
            (uint8_t)UCN_PATH_TRACE_STATUS_OK ||
        frame->source == 0U || frame->source == UCN_NODE_BROADCAST ||
        frame->destination == 0U || frame->destination == UCN_NODE_BROADCAST ||
        read_uint_be(frame->payload + UCN_PATH_TRACE_NODE_IDS_OFFSET,
                     descriptor->address_bytes) != frame->source) {
        node->stats.path_trace_rejected++;
        return UCN_ERR_MALFORMED;
    }
    if (node->path_trace_authorize == NULL ||
        !node->path_trace_authorize(node->path_trace_authorize_context,
                                    frame->source)) {
        node->stats.path_trace_rejected++;
        return UCN_ERR_ACCESS;
    }
    if (!take_control_rx_token(node, ingress_link,
                               UCN_CONTROL_RX_PATH_TRACE_REQUEST)) {
        return UCN_ERR_NO_SPACE;
    }
    trace_id = read_u32_be(frame->payload + UCN_PATH_TRACE_TRACE_ID_OFFSET);
    if (!append_path_trace_node(frame, node->config.node_id, payload,
                                &payload_length)) {
        (void)memcpy(payload, frame->payload, frame->payload_length);
        payload[UCN_PATH_TRACE_STATUS_OFFSET] =
            (uint8_t)UCN_PATH_TRACE_STATUS_TRUNCATED;
        node->stats.path_trace_rejected++;
        return send_path_trace_reply_on_link(node, ingress_link, frame->source,
                                             payload, frame->payload_length,
                                             frame->wire_profile);
    }

    if (frame->destination == node->config.node_id) {
        payload[UCN_PATH_TRACE_STATUS_OFFSET] = (uint8_t)UCN_PATH_TRACE_STATUS_OK;
        return send_path_trace_reply_on_link(node, ingress_link, frame->source,
                                             payload, payload_length,
                                             frame->wire_profile);
    }
    if (frame->hop_limit <= 1U) {
        payload[UCN_PATH_TRACE_STATUS_OFFSET] =
            (uint8_t)UCN_PATH_TRACE_STATUS_TTL_EXCEEDED;
        return send_path_trace_reply_on_link(node, ingress_link, frame->source,
                                             payload, payload_length,
                                             frame->wire_profile);
    }
    egress_link = find_link(node, frame->destination);
    if (egress_link == NULL || egress_link == ingress_link) {
        payload[UCN_PATH_TRACE_STATUS_OFFSET] =
            (uint8_t)UCN_PATH_TRACE_STATUS_NO_ROUTE;
        return send_path_trace_reply_on_link(node, ingress_link, frame->source,
                                             payload, payload_length,
                                             frame->wire_profile);
    }
    if (allocate_path_trace_reverse(node, frame->source, trace_id, ingress_link) == NULL) {
        payload[UCN_PATH_TRACE_STATUS_OFFSET] =
            (uint8_t)UCN_PATH_TRACE_STATUS_NO_ROUTE;
        node->stats.path_trace_rejected++;
        return send_path_trace_reply_on_link(node, ingress_link, frame->source,
                                             payload, payload_length,
                                             frame->wire_profile);
    }

    {
        ucn_frame_t forwarded = *frame;

        forwarded.payload = payload;
        forwarded.payload_length = payload_length;
        --forwarded.hop_limit;
        result = send_frame_on_link(node, egress_link, &forwarded);
    }
    return result;
}

/*
 * EN: Validates and processes `handle_path_trace_reply` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_path_trace_reply`。
 */
static ucn_result_t handle_path_trace_reply(ucn_node_t *node,
                                            ucn_link_t *ingress_link,
                                            const ucn_frame_t *frame)
{
    uint32_t trace_id;
    ucn_path_trace_reverse_t *reverse;
    ucn_result_t result;

    if (!path_trace_payload_is_valid(frame) || frame->source == 0U ||
        frame->source == UCN_NODE_BROADCAST || frame->destination == 0U ||
        frame->destination == UCN_NODE_BROADCAST) {
        node->stats.path_trace_rejected++;
        return UCN_ERR_MALFORMED;
    }
    if (frame->destination == node->config.node_id) {
        return complete_path_trace(node, frame);
    }
    trace_id = read_u32_be(frame->payload + UCN_PATH_TRACE_TRACE_ID_OFFSET);
    reverse = find_path_trace_reverse(node, frame->destination, trace_id);
    if (reverse == NULL || reverse->ingress_link == NULL ||
        reverse->ingress_link == ingress_link) {
        node->stats.path_trace_rejected++;
        return UCN_ERR_NOT_FOUND;
    }
    if (frame->hop_limit <= 1U) {
        return UCN_ERR_TTL;
    }
    {
        ucn_frame_t forwarded = *frame;

        --forwarded.hop_limit;
        result = send_frame_on_link(node, reverse->ingress_link, &forwarded);
    }
    reverse->occupied = false;
    return result;
}

/*
 * EN: Checks whether `node_snapshot_request_payload` satisfies the Lite/Full Node module's validity rules.
 * 中文：检查 `node_snapshot_request_payload` 是否满足 Lite/Full Node 模块的合法性规则。
 */
static bool node_snapshot_request_payload_is_valid(const ucn_frame_t *frame)
{
    size_t index;

    if (frame == NULL || frame->payload == NULL ||
        frame->payload_length != UCN_NODE_SNAPSHOT_REQUEST_PAYLOAD_BYTES ||
        (frame->flags & UCN_FRAME_FLAG_DIAGNOSTIC) == 0U ||
        frame->has_route_extension ||
        read_u32_be(frame->payload + UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET) == 0U) {
        return false;
    }
    for (index = UCN_NODE_SNAPSHOT_REQUEST_FLAGS_OFFSET;
         index < UCN_NODE_SNAPSHOT_REQUEST_PAYLOAD_BYTES; ++index) {
        if (frame->payload[index] != 0U) {
            return false;
        }
    }
    return true;
}

/*
 * EN: Checks whether `node_snapshot_reply_payload` satisfies the Lite/Full Node module's validity rules.
 * 中文：检查 `node_snapshot_reply_payload` 是否满足 Lite/Full Node 模块的合法性规则。
 */
static bool node_snapshot_reply_payload_is_valid(const ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor;
    ucn_node_id_t reported_node_id;
    size_t neighbor_offset;
    size_t flags_offset;

    if (frame == NULL || frame->payload == NULL ||
        (frame->flags & UCN_FRAME_FLAG_DIAGNOSTIC) == 0U ||
        frame->has_route_extension ||
        read_u32_be(frame->payload + UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET) == 0U) {
        return false;
    }
    descriptor = ucn_wire_profile_get_descriptor(frame->wire_profile);
    if (descriptor == NULL || frame->payload_length !=
                                  node_snapshot_reply_payload_size(
                                      frame->wire_profile)) {
        return false;
    }
    neighbor_offset = node_snapshot_reply_neighbor_count_offset(descriptor);
    flags_offset = node_snapshot_reply_flags_offset(descriptor);
    if (frame->payload[flags_offset + 1U] != 0U ||
        frame->payload[flags_offset + 2U] != 0U) {
        return false;
    }
    reported_node_id = read_uint_be(frame->payload + 4U,
                                    descriptor->address_bytes);
    return reported_node_id != 0U && reported_node_id != UCN_NODE_BROADCAST &&
           reported_node_id == frame->source &&
           frame->payload[neighbor_offset] <=
               UCN_MAX_NEIGHBORS;
}

/*
 * EN: Validates and submits `send_node_snapshot_reply_on_link` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_node_snapshot_reply_on_link` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_node_snapshot_reply_on_link(
    ucn_node_t *node,
    ucn_link_t *link,
    ucn_node_id_t destination,
    uint32_t query_id,
    ucn_wire_profile_t wire_profile)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(wire_profile);
    ucn_frame_t frame;
    uint8_t payload[UCN_NODE_SNAPSHOT_REPLY_MAX_PAYLOAD_BYTES];
    size_t payload_length = node_snapshot_reply_payload_size(wire_profile);
    ucn_result_t result;

    if (descriptor == NULL || payload_length > sizeof(payload)) {
        return UCN_ERR_CONFIG;
    }
    (void)memset(&frame, 0, sizeof(frame));
    (void)memset(payload, 0, sizeof(payload));
    write_u32_be(payload + UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET, query_id);
    write_uint_be(payload + 4U, descriptor->address_bytes,
                  node->config.node_id);
    payload[node_snapshot_reply_neighbor_count_offset(descriptor)] =
        (uint8_t)node->link_count;
    frame.message_type = UCN_MSG_NODE_SNAPSHOT_REPLY;
    frame.wire_profile = wire_profile;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.flags = UCN_FRAME_FLAG_DIAGNOSTIC;
    frame.hop_limit = node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = destination;
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = (uint16_t)payload_length;
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        return result;
    }
    frame.session_id = node->session_id;
    result = send_frame_on_link(node, link, &frame);
    if (result == UCN_OK) {
        node->stats.node_snapshot_replies_sent++;
    }
    return result;
}

/*
 * EN: Searches bounded Lite/Full Node state for `node_snapshot_pending`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `node_snapshot_pending`。
 */
static ucn_node_snapshot_pending_t *find_node_snapshot_pending(
    ucn_node_t *node,
    uint32_t query_id)
{
    size_t index;

    for (index = 0U; index < UCN_NODE_SNAPSHOT_PENDING_DEPTH; ++index) {
        if (node->node_snapshot_pending[index].occupied &&
            node->node_snapshot_pending[index].query_id == query_id) {
            return &node->node_snapshot_pending[index];
        }
    }
    return NULL;
}

/*
 * EN: Searches bounded Lite/Full Node state for `free_node_snapshot_pending`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `free_node_snapshot_pending`。
 */
static ucn_node_snapshot_pending_t *find_free_node_snapshot_pending(
    ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_NODE_SNAPSHOT_PENDING_DEPTH; ++index) {
        if (!node->node_snapshot_pending[index].occupied) {
            return &node->node_snapshot_pending[index];
        }
    }
    return NULL;
}

/*
 * EN: Searches bounded Lite/Full Node state for `node_snapshot_reverse`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `node_snapshot_reverse`。
 */
static ucn_node_snapshot_reverse_t *find_node_snapshot_reverse(
    ucn_node_t *node,
    ucn_node_id_t origin,
    uint32_t query_id)
{
    size_t index;

    for (index = 0U; index < UCN_NODE_SNAPSHOT_REVERSE_DEPTH; ++index) {
        ucn_node_snapshot_reverse_t *entry = &node->node_snapshot_reverse[index];

        if (entry->occupied &&
            ucn_deadline_expired(node->now_ms, entry->expires_at_ms)) {
            entry->occupied = false;
        }
        if (entry->occupied && entry->origin == origin && entry->query_id == query_id) {
            return entry;
        }
    }
    return NULL;
}

/*
 * EN: Allocates `allocate_node_snapshot_reverse` from fixed Lite/Full Node slots without heap use.
 * 中文：从 Lite/Full Node 的固定槽位分配 `allocate_node_snapshot_reverse`，不使用堆内存。
 */
static ucn_node_snapshot_reverse_t *allocate_node_snapshot_reverse(
    ucn_node_t *node,
    ucn_node_id_t origin,
    uint32_t query_id,
    ucn_link_t *ingress_link)
{
    size_t index;
    ucn_node_snapshot_reverse_t *free_slot = NULL;
    ucn_node_snapshot_reverse_t *existing =
        find_node_snapshot_reverse(node, origin, query_id);

    if (existing != NULL) {
        return existing;
    }
    for (index = 0U; index < UCN_NODE_SNAPSHOT_REVERSE_DEPTH; ++index) {
        if (!node->node_snapshot_reverse[index].occupied) {
            free_slot = &node->node_snapshot_reverse[index];
            break;
        }
    }
    if (free_slot == NULL) {
        return NULL;
    }
    (void)memset(free_slot, 0, sizeof(*free_slot));
    free_slot->occupied = true;
    free_slot->origin = origin;
    free_slot->query_id = query_id;
    free_slot->ingress_link = ingress_link;
    free_slot->expires_at_ms =
        ucn_deadline_from_now(node->now_ms, UCN_NODE_SNAPSHOT_TIMEOUT_MS);
    return free_slot;
}

/*
 * EN: Copies or submits `queue_node_snapshot_reply` to a bounded Lite/Full Node queue.
 * 中文：把 `queue_node_snapshot_reply` 复制或提交到固定容量的 Lite/Full Node 队列。
 */
static bool queue_node_snapshot_reply(ucn_node_t *node,
                                      ucn_node_id_t origin,
                                      uint32_t query_id,
                                      ucn_link_t *egress_link,
                                      ucn_wire_profile_t wire_profile)
{
    size_t index;
    ucn_node_snapshot_reply_pending_t *free_slot = NULL;

    for (index = 0U; index < UCN_NODE_SNAPSHOT_REPLY_QUEUE_DEPTH; ++index) {
        ucn_node_snapshot_reply_pending_t *entry = &node->node_snapshot_replies[index];

        if (entry->occupied && entry->origin == origin && entry->query_id == query_id) {
            return true;
        }
        if (!entry->occupied && free_slot == NULL) {
            free_slot = entry;
        }
    }
    if (free_slot == NULL) {
        return false;
    }
    (void)memset(free_slot, 0, sizeof(*free_slot));
    free_slot->occupied = true;
    free_slot->origin = origin;
    free_slot->query_id = query_id;
    free_slot->egress_link = egress_link;
    free_slot->wire_profile = wire_profile;
    {
        uint32_t delay_ms = (query_id ^ node->config.node_id) %
                            (UCN_NODE_SNAPSHOT_REPLY_JITTER_MS + UINT32_C(1));

        free_slot->due_at_ms =
            ucn_deadline_from_now(node->now_ms, delay_ms == 0U ? 1U : delay_ms);
    }
    free_slot->expires_at_ms =
        ucn_deadline_from_now(node->now_ms, UCN_NODE_SNAPSHOT_TIMEOUT_MS);
    return true;
}

/*
 * EN: Forwards or delivers `forward_node_snapshot_request` through the bounded Lite/Full Node path.
 * 中文：通过有界的 Lite/Full Node 路径转发或投递 `forward_node_snapshot_request`。
 */
static ucn_result_t forward_node_snapshot_request(ucn_node_t *node,
                                                   ucn_link_t *ingress_link,
                                                   const ucn_frame_t *frame)
{
    ucn_frame_t forwarded = *frame;
    size_t index;
    size_t sent_count = 0U;
    ucn_result_t last_error = UCN_ERR_NOT_FOUND;

    if (frame->hop_limit <= 1U) {
        return UCN_ERR_TTL;
    }
    --forwarded.hop_limit;
    for (index = 0U; index < node->link_count; ++index) {
        ucn_result_t result;

        if (node->links[index] == ingress_link ||
            !link_is_candidate_eligible(node, node->links[index])) {
            continue;
        }
        result = send_frame_on_link(node, node->links[index], &forwarded);
        if (result == UCN_OK) {
            ++sent_count;
        } else {
            last_error = result;
        }
    }
    return sent_count != 0U ? UCN_OK : last_error;
}

/*
 * EN: Checks or removes expired `expire_node_snapshot_state` state in Lite/Full Node.
 * 中文：检查或移除 Lite/Full Node 中已过期的 `expire_node_snapshot_state` 状态。
 */
static void expire_node_snapshot_state(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_NODE_SNAPSHOT_REVERSE_DEPTH; ++index) {
        if (node->node_snapshot_reverse[index].occupied &&
            ucn_deadline_expired(
                now_ms, node->node_snapshot_reverse[index].expires_at_ms)) {
            node->node_snapshot_reverse[index].occupied = false;
        }
    }
    for (index = 0U; index < UCN_NODE_SNAPSHOT_REPLY_QUEUE_DEPTH; ++index) {
        if (node->node_snapshot_replies[index].occupied &&
            ucn_deadline_expired(
                now_ms, node->node_snapshot_replies[index].expires_at_ms)) {
            node->node_snapshot_replies[index].occupied = false;
            node->stats.node_snapshot_rejected++;
        }
    }
    for (index = 0U; index < UCN_NODE_SNAPSHOT_PENDING_DEPTH; ++index) {
        ucn_node_snapshot_pending_t *pending = &node->node_snapshot_pending[index];

        if (pending->occupied &&
            ucn_deadline_expired(now_ms, pending->deadline_ms)) {
            ucn_node_snapshot_result_t result;
            ucn_node_snapshot_handler_t handler = pending->handler;
            void *context = pending->context;

            (void)memset(&result, 0, sizeof(result));
            result.status = pending->truncated ? UCN_NODE_SNAPSHOT_STATUS_TRUNCATED :
                                                UCN_NODE_SNAPSHOT_STATUS_COMPLETE;
            result.query_id = pending->query_id;
            result.node_count = pending->node_count;
            if (pending->node_count != 0U) {
                (void)memcpy(result.entries, pending->entries,
                             (size_t)pending->node_count * sizeof(result.entries[0]));
            }
            pending->occupied = false;
            node->stats.node_snapshot_completed++;
            if (result.status == UCN_NODE_SNAPSHOT_STATUS_TRUNCATED) {
                node->stats.node_snapshot_result_truncated++;
            }
            if (handler != NULL) {
                handler(context, &result);
            }
        }
    }
}

/*
 * EN: Validates and submits `send_due_node_snapshot_reply` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_due_node_snapshot_reply` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_due_node_snapshot_reply(ucn_node_t *node,
                                                  uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_NODE_SNAPSHOT_REPLY_QUEUE_DEPTH; ++index) {
        ucn_node_snapshot_reply_pending_t *pending = &node->node_snapshot_replies[index];
        ucn_result_t result;

        if (!pending->occupied ||
            !ucn_deadline_expired(now_ms, pending->due_at_ms)) {
            continue;
        }
        pending->occupied = false;
        result = send_node_snapshot_reply_on_link(node, pending->egress_link,
                                                  pending->origin,
                                                  pending->query_id,
                                                  pending->wire_profile);
        return result;
    }
    return UCN_ERR_NOT_FOUND;
}

/*
 * EN: Checks whether `policy_diagnostic_selector` satisfies the Lite/Full Node module's validity rules.
 * 中文：检查 `policy_diagnostic_selector` 是否满足 Lite/Full Node 模块的合法性规则。
 */
static bool policy_diagnostic_selector_is_valid(uint8_t section, uint8_t index)
{
    switch ((ucn_policy_diagnostic_section_t)section) {
    case UCN_POLICY_DIAGNOSTIC_SUMMARY:
        return index < 3U;
    case UCN_POLICY_DIAGNOSTIC_POLICY:
        return index < UCN_MAX_ROUTE_POLICIES;
    case UCN_POLICY_DIAGNOSTIC_PATH:
        return index < UCN_MAX_POLICY_PATHS;
    case UCN_POLICY_DIAGNOSTIC_FLOW:
        return index < UCN_MAX_POLICY_FLOWS;
    case UCN_POLICY_DIAGNOSTIC_LINK_QUALITY:
        return index < UCN_MAX_LINKS;
    default:
        return false;
    }
}

/*
 * EN: Checks whether `policy_diagnostic_request` satisfies the Lite/Full Node module's validity rules.
 * 中文：检查 `policy_diagnostic_request` 是否满足 Lite/Full Node 模块的合法性规则。
 */
static bool policy_diagnostic_request_is_valid(const ucn_frame_t *frame)
{
    const uint8_t section = frame == NULL || frame->payload == NULL ? UINT8_MAX :
                            frame->payload[UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET];
    const uint8_t index = frame == NULL || frame->payload == NULL ? 0U :
                          frame->payload[UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET];

    if (frame == NULL || frame->payload == NULL ||
        frame->payload_length != UCN_POLICY_DIAGNOSTIC_REQUEST_PAYLOAD_BYTES ||
        frame->traffic_class != UCN_TRAFFIC_Q1_REALTIME ||
        frame->flags != UCN_FRAME_FLAG_DIAGNOSTIC || frame->has_route_extension ||
        read_u32_be(frame->payload + UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET) == 0U ||
        frame->payload[UCN_POLICY_DIAGNOSTIC_REQUEST_RESERVED_OFFSET] != 0U ||
        frame->payload[UCN_POLICY_DIAGNOSTIC_REQUEST_RESERVED_OFFSET + 1U] != 0U) {
        return false;
    }
    return policy_diagnostic_selector_is_valid(section, index);
}

/*
 * EN: Checks whether `policy_diagnostic_reply` satisfies the Lite/Full Node module's validity rules.
 * 中文：检查 `policy_diagnostic_reply` 是否满足 Lite/Full Node 模块的合法性规则。
 */
static bool policy_diagnostic_reply_is_valid(const ucn_frame_t *frame)
{
    const uint8_t section = frame == NULL || frame->payload == NULL ? UINT8_MAX :
                            frame->payload[UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET];
    const uint8_t status = frame == NULL || frame->payload == NULL ? UINT8_MAX :
                           frame->payload[UCN_POLICY_DIAGNOSTIC_REPLY_STATUS_OFFSET];

    if (frame == NULL || frame->payload == NULL ||
        frame->payload_length != UCN_POLICY_DIAGNOSTIC_REPLY_PAYLOAD_BYTES ||
        frame->traffic_class != UCN_TRAFFIC_Q1_REALTIME ||
        frame->flags != UCN_FRAME_FLAG_DIAGNOSTIC || frame->has_route_extension ||
        read_u32_be(frame->payload + UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET) == 0U ||
        !policy_diagnostic_selector_is_valid(section,
            frame->payload[UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET]) ||
        status > (uint8_t)UCN_POLICY_DIAGNOSTIC_STATUS_EMPTY ||
        frame->payload[UCN_POLICY_DIAGNOSTIC_REPLY_RESERVED_OFFSET] != 0U) {
        return false;
    }
    if (status == (uint8_t)UCN_POLICY_DIAGNOSTIC_STATUS_EMPTY) {
        return true;
    }
    switch ((ucn_policy_diagnostic_section_t)section) {
    case UCN_POLICY_DIAGNOSTIC_POLICY:
        return frame->payload[UCN_POLICY_DIAGNOSTIC_RECORD_OFFSET + 6U] <=
               (uint8_t)UCN_ROUTE_POLICY_AUTO_BALANCE;
    case UCN_POLICY_DIAGNOSTIC_PATH:
        return frame->payload[UCN_POLICY_DIAGNOSTIC_RECORD_OFFSET + 10U] <=
               (uint8_t)UCN_POLICY_PATH_DOWN;
    case UCN_POLICY_DIAGNOSTIC_FLOW:
        return frame->payload[UCN_POLICY_DIAGNOSTIC_RECORD_OFFSET + 20U] <=
               (uint8_t)UCN_POLICY_PATH_DOWN;
    default:
        return true;
    }
}

/*
 * EN: Calculates `policy_diagnostic_quality_flags` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `policy_diagnostic_quality_flags`。
 */
static uint8_t policy_diagnostic_quality_flags(
    const ucn_policy_link_quality_snapshot_t *quality)
{
    uint8_t flags = 0U;

    if (quality == NULL) {
        return 0U;
    }
    if (quality->is_up) {
        flags |= UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_UP;
    }
    if (quality->route_cost_valid) {
        flags |= UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_ROUTE_COST;
    }
    if (quality->rtt_valid) {
        flags |= UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_RTT;
    }
    if (quality->tx_failure_rate_valid) {
        flags |= UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_TX_FAILURE;
    }
    if (quality->queue_pressure_valid) {
        flags |= UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_QUEUE_PRESSURE;
    }
    return flags;
}

/*
 * EN: Calculates `policy_diagnostic_write_quality` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `policy_diagnostic_write_quality`。
 */
static void policy_diagnostic_write_quality(
    uint8_t *record,
    size_t offset,
    const ucn_policy_link_quality_snapshot_t *quality)
{
    if (quality == NULL) {
        return;
    }
    write_u16_be(record + offset, quality->route_cost);
    write_u16_be(record + offset + 2U, quality->rtt_ewma_ms);
    write_u16_be(record + offset + 4U, quality->tx_failure_ewma_per_mille);
    write_u16_be(record + offset + 6U, quality->queue_pressure_ewma_per_mille);
}

/*
 * EN: Builds one bounded Policy diagnostic reply page.
 * 中文：构造一页有界的 Policy 诊断响应。
 */
static void policy_diagnostic_build_reply(
    const ucn_node_t *node,
    uint32_t request_id,
    ucn_policy_diagnostic_section_t section,
    uint8_t index,
    uint8_t *payload)
{
    uint8_t *record = payload + UCN_POLICY_DIAGNOSTIC_RECORD_OFFSET;
    ucn_policy_diagnostic_status_t status = UCN_POLICY_DIAGNOSTIC_STATUS_EMPTY;

    (void)memset(payload, 0, UCN_POLICY_DIAGNOSTIC_REPLY_PAYLOAD_BYTES);
    write_u32_be(payload + UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET, request_id);
    payload[UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET] = (uint8_t)section;
    payload[UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET] = index;

    switch (section) {
    case UCN_POLICY_DIAGNOSTIC_SUMMARY: {
        const ucn_policy_stats_t *stats = &node->policy_state.stats;
        const uint32_t *values = NULL;
        uint32_t page[6];
        size_t value_index;

        switch (index) {
        case 0U:
            page[0] = stats->policy_match_hits;
            page[1] = stats->pinned_strict_sends;
            page[2] = stats->pinned_strict_failures;
            page[3] = stats->pinned_failover_primary_sends;
            page[4] = stats->pinned_failover_backup_sends;
            page[5] = stats->pinned_failover_hard_failures;
            values = page;
            break;
        case 1U:
            page[0] = stats->pinned_failover_discovery_fallbacks;
            page[1] = stats->pinned_policy_config_errors;
            page[2] = stats->auto_balance_sends;
            page[3] = stats->auto_balance_flow_bindings;
            page[4] = stats->auto_balance_rebindings;
            page[5] = stats->auto_balance_congestion_rebindings;
            values = page;
            break;
        default:
            page[0] = stats->auto_balance_down_rebindings;
            page[1] = stats->auto_balance_selection_failures;
            page[2] = stats->flow_bindings_expired;
            page[3] = stats->quality_samples;
            page[4] = stats->quality_metrics_unavailable;
            page[5] = stats->quality_link_down;
            values = page;
            break;
        }
        for (value_index = 0U; value_index < 6U; ++value_index) {
            write_u32_be(record + value_index * sizeof(uint32_t), values[value_index]);
        }
        status = UCN_POLICY_DIAGNOSTIC_STATUS_OK;
        break;
    }
    case UCN_POLICY_DIAGNOSTIC_POLICY: {
        const ucn_route_policy_entry_t *entry =
            &node->policy_state.policies[index];

        if (entry->occupied) {
            write_u32_be(record, entry->config.key.destination);
            record[4] = entry->config.key.endpoint;
            record[5] = entry->config.key.traffic_class;
            record[6] = (uint8_t)entry->config.mode;
            record[7] = entry->config.allow_discovery_on_hard_failure ? 1U : 0U;
            write_u16_be(record + 8U, entry->config.primary_local_path_id);
            write_u16_be(record + 10U, entry->config.backup_local_path_id);
            write_u32_be(record + 12U, entry->config.balance_flow_lease_ms);
            write_u32_be(record + 16U, entry->match_hits);
            status = UCN_POLICY_DIAGNOSTIC_STATUS_OK;
        }
        break;
    }
    case UCN_POLICY_DIAGNOSTIC_PATH: {
        const ucn_policy_path_entry_t *entry = &node->policy_state.paths[index];

        if (entry->occupied) {
            ucn_link_t *active_bearer = resolve_egress_link((ucn_node_t *)node,
                                                             entry->egress_link);
            const ucn_policy_link_quality_snapshot_t *quality =
                active_bearer == NULL ? NULL :
                ucn_node_get_link_quality(node, active_bearer);

            write_u16_be(record, entry->local_path_id);
            write_u32_be(record + 2U, entry->wire_path_id);
            write_u32_be(record + 6U, entry->destination);
            record[10] = (uint8_t)entry->state;
            record[11] = entry->congestion_samples;
            record[12] = entry->egress_link == NULL ? 0U : entry->egress_link->link_id;
            record[13] = active_bearer == NULL ? 0U : active_bearer->link_id;
            record[14] = policy_diagnostic_quality_flags(quality);
            policy_diagnostic_write_quality(record, 16U, quality);
            status = UCN_POLICY_DIAGNOSTIC_STATUS_OK;
        }
        break;
    }
    case UCN_POLICY_DIAGNOSTIC_FLOW: {
        const ucn_policy_flow_binding_t *entry = &node->policy_state.flows[index];

        if (entry->occupied &&
            !ucn_deadline_expired(node->now_ms, entry->expires_at_ms)) {
            const ucn_policy_path_entry_t *path =
                ucn_node_find_policy_path(node, entry->local_path_id);
            ucn_link_t *active_bearer = path == NULL ? NULL :
                resolve_egress_link((ucn_node_t *)node, path->egress_link);

            write_u32_be(record, entry->key.destination);
            record[4] = entry->key.endpoint;
            record[5] = (uint8_t)entry->key.traffic_class;
            write_u16_be(record + 6U, entry->local_path_id);
            write_u32_be(record + 8U, entry->expires_at_ms);
            write_u32_be(record + 12U, entry->last_used_at_ms);
            write_u32_be(record + 16U, entry->expires_at_ms - node->now_ms);
            record[20] = path == NULL ? (uint8_t)UCN_POLICY_PATH_EMPTY :
                         (uint8_t)path->state;
            record[21] = active_bearer == NULL ? 0U : active_bearer->link_id;
            status = UCN_POLICY_DIAGNOSTIC_STATUS_OK;
        }
        break;
    }
    case UCN_POLICY_DIAGNOSTIC_LINK_QUALITY: {
        const ucn_policy_link_quality_snapshot_t *quality =
            &node->policy_state.quality[index];

        if (quality->occupied) {
            record[0] = quality->link == NULL ? 0U : quality->link->link_id;
            record[1] = policy_diagnostic_quality_flags(quality);
            policy_diagnostic_write_quality(record, 4U, quality);
            write_u32_be(record + 12U, quality->sampled_at_ms);
            status = UCN_POLICY_DIAGNOSTIC_STATUS_OK;
        }
        break;
    }
    default:
        break;
    }
    payload[UCN_POLICY_DIAGNOSTIC_REPLY_STATUS_OFFSET] = (uint8_t)status;
}

/*
 * EN: Reads and validates `policy_diagnostic_decode_result` from the canonical Lite/Full Node representation.
 * 中文：从规范的 Lite/Full Node 表示中读取并验证 `policy_diagnostic_decode_result`。
 */
static void policy_diagnostic_decode_result(
    ucn_policy_diagnostic_result_t *result,
    ucn_node_id_t source,
    const uint8_t *payload)
{
    const uint8_t *record = payload + UCN_POLICY_DIAGNOSTIC_RECORD_OFFSET;
    const uint8_t flags = record[14];

    (void)memset(result, 0, sizeof(*result));
    result->request_id = read_u32_be(payload + UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET);
    result->node_id = source;
    result->section = (ucn_policy_diagnostic_section_t)
        payload[UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET];
    result->index = payload[UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET];
    result->status = (ucn_policy_diagnostic_status_t)
        payload[UCN_POLICY_DIAGNOSTIC_REPLY_STATUS_OFFSET];
    if (result->status != UCN_POLICY_DIAGNOSTIC_STATUS_OK) {
        return;
    }

    switch (result->section) {
    case UCN_POLICY_DIAGNOSTIC_SUMMARY: {
        size_t value_index;

        for (value_index = 0U; value_index < 6U; ++value_index) {
            result->record.summary.counters[value_index] =
                read_u32_be(record + value_index * sizeof(uint32_t));
        }
        break;
    }
    case UCN_POLICY_DIAGNOSTIC_POLICY:
        result->record.policy.key.destination = read_u32_be(record);
        result->record.policy.key.endpoint = record[4];
        result->record.policy.key.traffic_class = record[5];
        result->record.policy.mode = (ucn_route_policy_mode_t)record[6];
        result->record.policy.allow_discovery_on_hard_failure = record[7] != 0U;
        result->record.policy.primary_local_path_id = read_u16_be(record + 8U);
        result->record.policy.backup_local_path_id = read_u16_be(record + 10U);
        result->record.policy.balance_flow_lease_ms = read_u32_be(record + 12U);
        result->record.policy.match_hits = read_u32_be(record + 16U);
        break;
    case UCN_POLICY_DIAGNOSTIC_PATH:
        result->record.path.local_path_id = read_u16_be(record);
        result->record.path.wire_path_id = read_u32_be(record + 2U);
        result->record.path.destination = read_u32_be(record + 6U);
        result->record.path.state = (ucn_policy_path_state_t)record[10];
        result->record.path.congestion_samples = record[11];
        result->record.path.configured_egress_link_id = record[12];
        result->record.path.active_bearer_link_id = record[13];
        result->record.path.active_bearer_is_up =
            (flags & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_UP) != 0U;
        result->record.path.route_cost_valid =
            (flags & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_ROUTE_COST) != 0U;
        result->record.path.rtt_valid =
            (flags & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_RTT) != 0U;
        result->record.path.tx_failure_rate_valid =
            (flags & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_TX_FAILURE) != 0U;
        result->record.path.queue_pressure_valid =
            (flags & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_QUEUE_PRESSURE) != 0U;
        result->record.path.route_cost = read_u16_be(record + 16U);
        result->record.path.rtt_ewma_ms = read_u16_be(record + 18U);
        result->record.path.tx_failure_ewma_per_mille = read_u16_be(record + 20U);
        result->record.path.queue_pressure_ewma_per_mille = read_u16_be(record + 22U);
        break;
    case UCN_POLICY_DIAGNOSTIC_FLOW:
        result->record.flow.key.destination = read_u32_be(record);
        result->record.flow.key.endpoint = record[4];
        result->record.flow.key.traffic_class = (ucn_traffic_class_t)record[5];
        result->record.flow.local_path_id = read_u16_be(record + 6U);
        result->record.flow.expires_at_ms = read_u32_be(record + 8U);
        result->record.flow.last_used_at_ms = read_u32_be(record + 12U);
        result->record.flow.remaining_ms = read_u32_be(record + 16U);
        result->record.flow.path_state = (ucn_policy_path_state_t)record[20];
        result->record.flow.active_bearer_link_id = record[21];
        break;
    case UCN_POLICY_DIAGNOSTIC_LINK_QUALITY:
        result->record.link_quality.link_id = record[0];
        result->record.link_quality.is_up =
            (record[1] & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_UP) != 0U;
        result->record.link_quality.route_cost_valid =
            (record[1] & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_ROUTE_COST) != 0U;
        result->record.link_quality.rtt_valid =
            (record[1] & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_RTT) != 0U;
        result->record.link_quality.tx_failure_rate_valid =
            (record[1] & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_TX_FAILURE) != 0U;
        result->record.link_quality.queue_pressure_valid =
            (record[1] & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_QUEUE_PRESSURE) != 0U;
        result->record.link_quality.route_cost = read_u16_be(record + 4U);
        result->record.link_quality.rtt_ewma_ms = read_u16_be(record + 6U);
        result->record.link_quality.tx_failure_ewma_per_mille = read_u16_be(record + 8U);
        result->record.link_quality.queue_pressure_ewma_per_mille =
            read_u16_be(record + 10U);
        result->record.link_quality.sampled_at_ms = read_u32_be(record + 12U);
        break;
    default:
        break;
    }
}

/*
 * EN: Searches bounded Lite/Full Node state for `policy_diagnostic_pending`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `policy_diagnostic_pending`。
 */
static ucn_policy_diagnostic_pending_t *find_policy_diagnostic_pending(
    ucn_node_t *node,
    uint32_t request_id)
{
    size_t index;

    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH; ++index) {
        if (node->policy_diagnostic_pending[index].occupied &&
            node->policy_diagnostic_pending[index].request_id == request_id) {
            return &node->policy_diagnostic_pending[index];
        }
    }
    return NULL;
}

/*
 * EN: Searches bounded Lite/Full Node state for `free_policy_diagnostic_pending`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `free_policy_diagnostic_pending`。
 */
static ucn_policy_diagnostic_pending_t *find_free_policy_diagnostic_pending(
    ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH; ++index) {
        if (!node->policy_diagnostic_pending[index].occupied) {
            return &node->policy_diagnostic_pending[index];
        }
    }
    return NULL;
}

/*
 * EN: Copies or submits `queue_policy_diagnostic_reply` to a bounded Lite/Full Node queue.
 * 中文：把 `queue_policy_diagnostic_reply` 复制或提交到固定容量的 Lite/Full Node 队列。
 */
static bool queue_policy_diagnostic_reply(ucn_node_t *node,
                                          ucn_node_id_t destination,
                                          const uint8_t *payload)
{
    size_t index;

    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_REPLY_QUEUE_DEPTH; ++index) {
        ucn_policy_diagnostic_reply_pending_t *entry =
            &node->policy_diagnostic_replies[index];

        if (!entry->occupied) {
            (void)memset(entry, 0, sizeof(*entry));
            entry->occupied = true;
            entry->destination = destination;
            entry->expires_at_ms =
                ucn_deadline_from_now(node->now_ms,
                                      UCN_POLICY_DIAGNOSTIC_TIMEOUT_MS);
            (void)memcpy(entry->payload, payload, sizeof(entry->payload));
            return true;
        }
    }
    return false;
}

/*
 * EN: Checks or removes expired `expire_policy_diagnostic_state` state in Lite/Full Node.
 * 中文：检查或移除 Lite/Full Node 中已过期的 `expire_policy_diagnostic_state` 状态。
 */
static void expire_policy_diagnostic_state(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_REPLY_QUEUE_DEPTH; ++index) {
        ucn_policy_diagnostic_reply_pending_t *entry =
            &node->policy_diagnostic_replies[index];

        if (entry->occupied &&
            ucn_deadline_expired(now_ms, entry->expires_at_ms)) {
            entry->occupied = false;
            node->stats.policy_diagnostic_rejected++;
        }
    }
    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH; ++index) {
        ucn_policy_diagnostic_pending_t *pending =
            &node->policy_diagnostic_pending[index];

        if (pending->occupied &&
            ucn_deadline_expired(now_ms, pending->deadline_ms)) {
            ucn_policy_diagnostic_result_t result;
            ucn_policy_diagnostic_handler_t handler = pending->handler;
            void *context = pending->context;

            (void)memset(&result, 0, sizeof(result));
            result.request_id = pending->request_id;
            result.node_id = pending->destination;
            result.section = pending->section;
            result.index = pending->index;
            result.status = UCN_POLICY_DIAGNOSTIC_STATUS_TIMEOUT;
            pending->occupied = false;
            node->stats.policy_diagnostic_timeouts++;
            if (handler != NULL) {
                handler(context, &result);
            }
        }
    }
}

/*
 * EN: Validates and submits `send_policy_diagnostic_frame_on_link` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_policy_diagnostic_frame_on_link` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_policy_diagnostic_frame_on_link(
    ucn_node_t *node,
    ucn_link_t *link,
    ucn_node_id_t destination,
    uint8_t message_type,
    const uint8_t *payload,
    uint16_t payload_length)
{
    ucn_frame_t frame;
    ucn_result_t result;

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = message_type;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.flags = UCN_FRAME_FLAG_DIAGNOSTIC;
    frame.hop_limit = node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = destination;
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = payload_length;
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        return result;
    }
    frame.session_id = node->session_id;
    return send_frame_on_link(node, link, &frame);
}

/*
 * EN: Validates and submits `send_due_policy_diagnostic_request` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_due_policy_diagnostic_request` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_due_policy_diagnostic_request(ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH; ++index) {
        ucn_policy_diagnostic_pending_t *pending =
            &node->policy_diagnostic_pending[index];
        uint8_t payload[UCN_POLICY_DIAGNOSTIC_REQUEST_PAYLOAD_BYTES];
        ucn_link_t *link;

        if (!pending->occupied || pending->sent) {
            continue;
        }
        link = find_link(node, pending->destination);
        if (link == NULL) {
            return UCN_ERR_NOT_FOUND;
        }
        (void)memset(payload, 0, sizeof(payload));
        write_u32_be(payload + UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET,
                     pending->request_id);
        payload[UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET] = (uint8_t)pending->section;
        payload[UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET] = pending->index;
        /* Mark before send: virtual transports can synchronously complete the
         * reply and clear this same slot.  A failed attempt is not retried
         * outside the independent rate budget. */
        pending->sent = true;
        node->stats.policy_diagnostic_requests_sent++;
        return send_policy_diagnostic_frame_on_link(
            node, link, pending->destination, UCN_MSG_POLICY_DIAGNOSTIC_REQ,
            payload, (uint16_t)sizeof(payload));
    }
    return UCN_ERR_NOT_FOUND;
}

/*
 * EN: Validates and submits `send_due_policy_diagnostic_reply` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_due_policy_diagnostic_reply` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_due_policy_diagnostic_reply(ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_REPLY_QUEUE_DEPTH; ++index) {
        ucn_policy_diagnostic_reply_pending_t *pending =
            &node->policy_diagnostic_replies[index];
        ucn_link_t *link;
        ucn_result_t result;

        if (!pending->occupied) {
            continue;
        }
        link = find_link(node, pending->destination);
        if (link == NULL) {
            pending->occupied = false;
            node->stats.policy_diagnostic_rejected++;
            return UCN_ERR_NOT_FOUND;
        }
        pending->occupied = false;
        result = send_policy_diagnostic_frame_on_link(
            node, link, pending->destination, UCN_MSG_POLICY_DIAGNOSTIC_REPLY,
            pending->payload, (uint16_t)sizeof(pending->payload));
        if (result == UCN_OK) {
            node->stats.policy_diagnostic_replies_sent++;
        }
        return result;
    }
    return UCN_ERR_NOT_FOUND;
}

/*
 * EN: Validates and processes `handle_policy_diagnostic_request` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_policy_diagnostic_request`。
 */
static ucn_result_t handle_policy_diagnostic_request(
    ucn_node_t *node,
    const ucn_frame_t *frame)
{
    uint8_t payload[UCN_POLICY_DIAGNOSTIC_REPLY_PAYLOAD_BYTES];
    uint32_t request_id;

    if (!policy_diagnostic_request_is_valid(frame) ||
        frame->destination != node->config.node_id ||
        node->policy_diagnostic_authorize == NULL ||
        !node->policy_diagnostic_authorize(node->policy_diagnostic_authorize_context,
                                           frame->source)) {
        node->stats.policy_diagnostic_rejected++;
        return UCN_ERR_ACCESS;
    }
    if (!take_policy_diagnostic_token(node)) {
        return UCN_ERR_NO_SPACE;
    }
    request_id = read_u32_be(frame->payload + UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET);
    policy_diagnostic_build_reply(node, request_id,
        (ucn_policy_diagnostic_section_t)
            frame->payload[UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET],
        frame->payload[UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET], payload);
    if (!queue_policy_diagnostic_reply(node, frame->source, payload)) {
        node->stats.policy_diagnostic_rejected++;
        return UCN_ERR_NO_SPACE;
    }
    node->stats.policy_diagnostic_requests_received++;
    return UCN_OK;
}

/*
 * EN: Validates and processes `handle_policy_diagnostic_reply` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_policy_diagnostic_reply`。
 */
static ucn_result_t handle_policy_diagnostic_reply(
    ucn_node_t *node,
    const ucn_frame_t *frame)
{
    uint32_t request_id;
    ucn_policy_diagnostic_pending_t *pending;
    ucn_policy_diagnostic_result_t result;
    ucn_policy_diagnostic_handler_t handler;
    void *context;

    if (!policy_diagnostic_reply_is_valid(frame) ||
        frame->destination != node->config.node_id) {
        node->stats.policy_diagnostic_rejected++;
        return UCN_ERR_MALFORMED;
    }
    request_id = read_u32_be(frame->payload + UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET);
    pending = find_policy_diagnostic_pending(node, request_id);
    if (pending == NULL || !pending->sent ||
        pending->destination != frame->source ||
        pending->section != (ucn_policy_diagnostic_section_t)
                                frame->payload[UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET] ||
        pending->index != frame->payload[UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET]) {
        node->stats.policy_diagnostic_rejected++;
        return UCN_ERR_NOT_FOUND;
    }
    policy_diagnostic_decode_result(&result, frame->source, frame->payload);
    handler = pending->handler;
    context = pending->context;
    pending->occupied = false;
    node->stats.policy_diagnostic_replies_received++;
    node->stats.policy_diagnostic_completed++;
    if (handler != NULL) {
        handler(context, &result);
    }
    return UCN_OK;
}

/*
 * EN: Completes `complete_node_snapshot_reply` and records its terminal Lite/Full Node result.
 * 中文：完成 `complete_node_snapshot_reply` 并记录其 Lite/Full Node 终态结果。
 */
static ucn_result_t complete_node_snapshot_reply(ucn_node_t *node,
                                                  const ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(frame->wire_profile);
    uint32_t query_id;
    ucn_node_snapshot_pending_t *pending;
    size_t index;

    query_id = read_u32_be(frame->payload + UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET);
    pending = find_node_snapshot_pending(node, query_id);
    if (pending == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    for (index = 0U; index < pending->node_count; ++index) {
        if (pending->entries[index].node_id == frame->source) {
            return UCN_OK;
        }
    }
    if (pending->node_count >= pending->result_limit) {
        pending->truncated = true;
        return UCN_OK;
    }
    pending->entries[pending->node_count].node_id = frame->source;
    pending->entries[pending->node_count].direct_link_count =
        frame->payload[node_snapshot_reply_neighbor_count_offset(descriptor)];
    pending->entries[pending->node_count].flags =
        frame->payload[node_snapshot_reply_flags_offset(descriptor)];
    ++pending->node_count;
    node->stats.node_snapshot_replies_received++;
    return UCN_OK;
}

/*
 * EN: Validates and processes `handle_node_snapshot_request` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_node_snapshot_request`。
 */
static ucn_result_t handle_node_snapshot_request(ucn_node_t *node,
                                                  ucn_link_t *ingress_link,
                                                  const ucn_frame_t *frame)
{
    uint32_t query_id;
    ucn_result_t result;

    if (!node_snapshot_request_payload_is_valid(frame) ||
        frame->source == 0U || frame->source == UCN_NODE_BROADCAST ||
        frame->destination != UCN_NODE_BROADCAST ||
        node->node_snapshot_authorize == NULL ||
        !node->node_snapshot_authorize(node->node_snapshot_authorize_context,
                                       frame->source)) {
        node->stats.node_snapshot_rejected++;
        return UCN_ERR_ACCESS;
    }
    if (!take_node_snapshot_token(node)) {
        return UCN_ERR_NO_SPACE;
    }
    query_id = read_u32_be(frame->payload + UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET);
    if (allocate_node_snapshot_reverse(node, frame->source, query_id, ingress_link) == NULL) {
        node->stats.node_snapshot_rejected++;
        return UCN_ERR_NO_SPACE;
    }
    if (!queue_node_snapshot_reply(node, frame->source, query_id, ingress_link,
                                   frame->wire_profile)) {
        node->stats.node_snapshot_rejected++;
    }
    node->stats.node_snapshot_requests_received++;
    result = forward_node_snapshot_request(node, ingress_link, frame);
    return result == UCN_ERR_NOT_FOUND || result == UCN_ERR_TTL ? UCN_OK : result;
}

/*
 * EN: Validates and processes `handle_node_snapshot_reply` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_node_snapshot_reply`。
 */
static ucn_result_t handle_node_snapshot_reply(ucn_node_t *node,
                                                ucn_link_t *ingress_link,
                                                const ucn_frame_t *frame)
{
    uint32_t query_id;
    ucn_node_snapshot_reverse_t *reverse;
    ucn_result_t result;

    if (!node_snapshot_reply_payload_is_valid(frame) || frame->source == 0U ||
        frame->source == UCN_NODE_BROADCAST || frame->destination == 0U ||
        frame->destination == UCN_NODE_BROADCAST) {
        node->stats.node_snapshot_rejected++;
        return UCN_ERR_MALFORMED;
    }
    if (frame->destination == node->config.node_id) {
        return complete_node_snapshot_reply(node, frame);
    }
    query_id = read_u32_be(frame->payload + UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET);
    reverse = find_node_snapshot_reverse(node, frame->destination, query_id);
    if (reverse == NULL || reverse->ingress_link == NULL ||
        reverse->ingress_link == ingress_link) {
        node->stats.node_snapshot_rejected++;
        return UCN_ERR_NOT_FOUND;
    }
    if (frame->hop_limit <= 1U) {
        return UCN_ERR_TTL;
    }
    {
        ucn_frame_t forwarded = *frame;

        --forwarded.hop_limit;
        result = send_frame_on_link(node, reverse->ingress_link, &forwarded);
    }
    return result;
}

#endif

/*
 * EN: Updates `record_max_service_delay` in bounded Lite/Full Node state.
 * 中文：更新固定容量 Lite/Full Node 状态中的 `record_max_service_delay`。
 */
static void record_max_service_delay(uint32_t *maximum_ms, uint32_t delay_ms)
{
    if (delay_ms > *maximum_ms) {
        *maximum_ms = delay_ms;
    }
}

/*
 * EN: Validates and submits `send_due_heartbeat` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_due_heartbeat` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_due_heartbeat(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        ucn_neighbor_entry_t *entry = &node->neighbors[index];
        size_t bearer_index;

        if (entry->state != UCN_NEIGHBOR_ADMITTED &&
            entry->state != UCN_NEIGHBOR_SUSPECT) {
            continue;
        }
        for (bearer_index = 0U;
             bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR;
             ++bearer_index) {
            ucn_neighbor_bearer_t *bearer = &entry->bearers[bearer_index];
            uint8_t payload[UCN_HEARTBEAT_PAYLOAD_BYTES];
            uint32_t service_delay_ms = 0U;
            const bool first_heartbeat = !bearer->heartbeat_sent;
            ucn_result_t result;

            if (!bearer_is_active(bearer) || !link_is_usable(bearer->link) ||
                (bearer->heartbeat_sent &&
                 (uint32_t)(now_ms - bearer->last_heartbeat_sent_ms) <
                 link_heartbeat_interval_ms(bearer->link))) {
                continue;
            }
            if (bearer->heartbeat_sent) {
                service_delay_ms =
                    (uint32_t)(now_ms - bearer->last_heartbeat_sent_ms) -
                    link_heartbeat_interval_ms(bearer->link);
            }
            if (node->next_heartbeat_id == 0U) {
                node->next_heartbeat_id = 1U;
            }
            /* Scheduled Heartbeats are already bounded by one fixed interval
             * per admitted Bearer and UCN_MAX_LINKS.  They must not compete
             * with bursty RREQ/Probe/Activate traffic for the generic token
             * bucket, otherwise an unrelated control burst can create a
             * false liveness failure.  Peer RX retains its own Heartbeat
             * request token budget. */
            (void)memset(payload, 0, sizeof(payload));
            payload[0] = UCN_HEARTBEAT_REQUEST;
            write_u32_be(payload + 4U, node->next_heartbeat_id++);
            result = send_adaptive_control_on_link(
                node, bearer->link, entry->peer_node_id, UCN_MSG_HEARTBEAT,
                payload, (uint16_t)sizeof(payload));
            if (result == UCN_OK) {
                bearer->heartbeat_sent = true;
                /* Keep the first link-local liveness proof immediate, then
                 * shift the recurring cadence into a deterministic local
                 * phase.  No extra timer/state is required: wrap-safe
                 * elapsed arithmetic treats `now - phase` as an earlier
                 * successful send, so the next request is due after
                 * `interval - phase` and later requests retain that phase.
                 * 首个链路本地存活证明仍立即发送，随后把周期心跳平移到确定性
                 * 本地相位；借助回绕安全的 elapsed 算术，无需新增定时器或状态。 */
                bearer->last_heartbeat_sent_ms =
                    first_heartbeat ?
                        now_ms - initial_heartbeat_phase_ms(node, entry,
                                                           bearer) :
                        now_ms;
                node->stats.heartbeat_requests_sent++;
                record_max_service_delay(
                    &node->stats.max_heartbeat_service_delay_ms,
                    service_delay_ms);
            }
            return result;
        }
    }
    return UCN_ERR_NOT_FOUND;
}

/* Reuse the one-hop HEARTBEAT request/ACK wire format as a fixed-size Bearer
 * quality probe.  The candidate is addressed on its own Link, so a relay
 * never needs to parse or forward it.  State is installed before the send:
 * virtual Links and some Drivers may synchronously deliver the ACK. */
/*
 * EN: Validates and submits `send_due_bearer_quality_probe` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_due_bearer_quality_probe` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_due_bearer_quality_probe(ucn_node_t *node,
                                                   uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        ucn_neighbor_entry_t *entry = &node->neighbors[index];
        ucn_neighbor_bearer_t *primary;
        ucn_neighbor_bearer_t *candidate;
        uint8_t payload[UCN_HEARTBEAT_PAYLOAD_BYTES];
        uint32_t probe_id;
        uint32_t service_delay_ms;
        ucn_result_t result;

        if (entry->state != UCN_NEIGHBOR_ADMITTED &&
            entry->state != UCN_NEIGHBOR_SUSPECT) {
            continue;
        }
        primary = select_neighbor_bearer(node, entry);
        candidate = find_better_neighbor_bearer(node, entry, primary);
        if (candidate == NULL ||
            candidate->quality_better_samples <
                UCN_BEARER_QUALITY_STABLE_SAMPLES ||
            candidate->quality_probe_acks >=
                UCN_BEARER_QUALITY_PROBE_REQUIRED_ACKS ||
            candidate->quality_probes_sent >=
                UCN_BEARER_QUALITY_PROBE_MAX_ATTEMPTS) {
            continue;
        }
        if (candidate->quality_probe_pending) {
            const uint32_t elapsed_ms =
                (uint32_t)(now_ms - candidate->quality_probe_sent_at_ms);

            if (elapsed_ms <
                UCN_BEARER_QUALITY_PROBE_INTERVAL_MS) {
                continue;
            }
            service_delay_ms =
                elapsed_ms - UCN_BEARER_QUALITY_PROBE_INTERVAL_MS;
            candidate->quality_probe_pending = false;
        } else if (candidate->quality_probes_sent == 0U) {
            service_delay_ms =
                (uint32_t)(now_ms - entry->last_bearer_quality_sample_ms);
        } else {
            /* An ACK makes the next required Probe immediately eligible.
             * Using the previous send time is conservative if that ACK was
             * delivered asynchronously after the send. */
            service_delay_ms =
                (uint32_t)(now_ms - candidate->quality_probe_sent_at_ms);
        }
        if (node->next_heartbeat_id == 0U) {
            node->next_heartbeat_id = 1U;
        }
        if (!take_control_token(node)) {
            return UCN_ERR_NO_SPACE;
        }
        probe_id = node->next_heartbeat_id++;
        (void)memset(payload, 0, sizeof(payload));
        payload[0] = UCN_HEARTBEAT_REQUEST;
        write_u32_be(payload + 4U, probe_id);
        candidate->quality_probe_id = probe_id;
        candidate->quality_probe_sent_at_ms = now_ms;
        candidate->quality_probe_pending = true;
        candidate->quality_probes_sent++;
        result = send_adaptive_control_on_link(
            node, candidate->link, entry->peer_node_id, UCN_MSG_HEARTBEAT,
            payload, (uint16_t)sizeof(payload));
        if (result == UCN_OK) {
            node->stats.bearer_quality_probes_sent++;
            record_max_service_delay(&node->stats.max_probe_service_delay_ms,
                                     service_delay_ms);
        } else {
            candidate->quality_probe_pending = false;
        }
        return result;
    }
    return UCN_ERR_NOT_FOUND;
}

/*
 * EN: Validates and processes `handle_heartbeat` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_heartbeat`。
 */
static ucn_result_t handle_heartbeat(ucn_node_t *node,
                                     ucn_link_t *ingress_link,
                                     const ucn_frame_t *frame)
{
    ucn_neighbor_entry_t *entry = find_neighbor_by_link(node, ingress_link);
    ucn_neighbor_bearer_t *bearer = find_neighbor_bearer(entry, ingress_link);
    uint8_t response[UCN_HEARTBEAT_PAYLOAD_BYTES];
    ucn_result_t result;

    if (entry == NULL || bearer == NULL || !bearer_is_active(bearer)) {
        return UCN_ERR_ACCESS;
    }
    if (frame->payload_length != UCN_HEARTBEAT_PAYLOAD_BYTES ||
        frame->destination != node->config.node_id || frame->hop_limit != 1U ||
        frame->source != entry->peer_node_id || frame->payload[1] != 0U ||
        frame->payload[2] != 0U || frame->payload[3] != 0U ||
        (frame->payload[0] != UCN_HEARTBEAT_REQUEST &&
         frame->payload[0] != UCN_HEARTBEAT_ACK)) {
        return UCN_ERR_MALFORMED;
    }

    if (frame->payload[0] == UCN_HEARTBEAT_REQUEST &&
        !take_control_rx_token(node, ingress_link,
                               UCN_CONTROL_RX_HEARTBEAT_REQUEST)) {
        return UCN_ERR_NO_SPACE;
    }

    touch_neighbor(node, ingress_link);
    node->stats.heartbeat_received++;
    if (frame->payload[0] == UCN_HEARTBEAT_ACK) {
        if (bearer->quality_probe_pending &&
            bearer->quality_probe_id == read_u32_be(frame->payload + 4U)) {
            bearer->quality_probe_pending = false;
            if (bearer->quality_probe_acks <
                UCN_BEARER_QUALITY_PROBE_REQUIRED_ACKS) {
                bearer->quality_probe_acks++;
                node->stats.bearer_quality_probe_acks_received++;
            }
        }
        return UCN_OK;
    }

#if !UCN_TEST_NODE_HEARTBEAT_ACK_ENABLED
    /* Diagnostic-only: the request has still passed security, duplicate,
     * peer-budget and liveness handling; stop immediately before the nested
     * Link send used to return its ACK. */
    return UCN_OK;
#endif

    (void)memcpy(response, frame->payload, sizeof(response));
    response[0] = UCN_HEARTBEAT_ACK;
    result = send_adaptive_control_on_link(
        node, ingress_link, frame->source, UCN_MSG_HEARTBEAT, response,
        (uint16_t)sizeof(response));
    if (result == UCN_OK) {
        node->stats.heartbeat_acks_sent++;
    }
    return result;
}

#if UCN_FEATURE_CANDIDATE_ROUTING
/*
 * EN: Searches bounded Lite/Full Node state for `candidate_link`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `candidate_link`。
 */
static ucn_link_t *find_candidate_link(ucn_node_t *node,
                                       ucn_node_id_t destination,
                                       uint32_t candidate_id)
{
    ucn_candidate_route_t *candidate =
        find_candidate_route(node, destination, candidate_id);
    ucn_link_t *egress_link;

    if (candidate == NULL) {
        return NULL;
    }
    egress_link = resolve_egress_link(node, candidate->egress_link);
    return egress_link == NULL || !link_is_candidate_eligible(node, egress_link) ?
           NULL : egress_link;
}

/*
 * EN: Calculates `allocate_route_epoch` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `allocate_route_epoch`。
 */
static uint16_t allocate_route_epoch(ucn_node_t *node,
                                     ucn_node_id_t destination,
                                     ucn_wire_profile_t wire_profile)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(wire_profile);
    const uint16_t maximum =
        descriptor != NULL && descriptor->route_epoch_bytes == 1U ?
            UINT16_C(0x00FF) : UINT16_MAX;
    ucn_route_entry_t *route = find_active_route(node, destination);

    for (;;) {
        uint16_t route_epoch;

        if (node->next_route_epoch == 0U ||
            node->next_route_epoch >= maximum) {
            node->next_route_epoch = 1U;
        }
        route_epoch = node->next_route_epoch++;
        if (route == NULL || (route_epoch != route->route_epoch &&
                              (!route->previous_valid ||
                               route_epoch != route->previous_route_epoch))) {
            return route_epoch;
        }
    }
}

/*
 * EN: Validates and submits `send_due_path_probe` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_due_path_probe` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_due_path_probe(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        ucn_candidate_route_t *candidate = &node->candidates[index];
        uint8_t payload[UCN_PATH_PROBE_PAYLOAD_BYTES];
        ucn_link_t *egress_link;
        uint32_t service_delay_ms;
        ucn_result_t result;

        if (!candidate->valid || !candidate->originated_here) {
            continue;
        }
        egress_link = find_candidate_link(node, candidate->destination,
                                          candidate->candidate_id);
        if (egress_link == NULL) {
            continue;
        }
        if (candidate->probes_acked >= UCN_PATH_PROBE_REQUIRED_ACKS) {
            if (candidate->activation_sent) {
                continue;
            }
            if (candidate->route_epoch == 0U) {
                candidate->route_epoch = allocate_route_epoch(node,
                                                               candidate->destination,
                                                               candidate->wire_profile);
            }
            if (!take_control_token(node)) {
                return UCN_ERR_NO_SPACE;
            }
            write_u32_be(payload, candidate->candidate_id);
            write_u16_be(payload + 4U, candidate->route_epoch);
            result = send_control_on_link_profile(
                node, egress_link, candidate->destination,
                UCN_MSG_PATH_ACTIVATE, payload,
                UCN_PATH_ACTIVATE_PAYLOAD_BYTES, candidate->wire_profile);
            if (result == UCN_OK) {
                candidate->activation_sent = true;
            }
            return result;
        }
        if (candidate->probes_sent >= UCN_PATH_PROBE_REQUIRED_ACKS ||
            (candidate->next_probe_at_ms != 0U &&
             !ucn_deadline_expired(now_ms, candidate->next_probe_at_ms))) {
            continue;
        }

        service_delay_ms = candidate->next_probe_at_ms == 0U ?
            0U : (uint32_t)(now_ms - candidate->next_probe_at_ms);

        if (!take_control_token(node)) {
            return UCN_ERR_NO_SPACE;
        }

        write_u32_be(payload, candidate->candidate_id);
        write_u32_be(payload + 4U, (uint32_t)candidate->probes_sent + 1U);
        write_u32_be(payload + 8U, now_ms);
        result = send_control_on_link_profile(
            node, egress_link, candidate->destination, UCN_MSG_PATH_PROBE,
            payload, (uint16_t)sizeof(payload), candidate->wire_profile);
        if (result == UCN_OK) {
            candidate->probes_sent++;
            candidate->next_probe_at_ms =
                ucn_deadline_from_now(now_ms, UCN_PATH_PROBE_INTERVAL_MS);
            node->stats.path_probes_sent++;
            record_max_service_delay(&node->stats.max_probe_service_delay_ms,
                                     service_delay_ms);
        }
        return result;
    }
    return UCN_ERR_NOT_FOUND;
}

/*
 * EN: Validates and processes `handle_path_probe` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_path_probe`。
 */
static ucn_result_t handle_path_probe(ucn_node_t *node,
                                      const ucn_frame_t *frame)
{
    uint32_t candidate_id;
    ucn_link_t *egress_link;

    if (frame->payload_length != UCN_PATH_PROBE_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    candidate_id = read_u32_be(frame->payload);
    if (candidate_id == 0U || read_u32_be(frame->payload + 4U) == 0U) {
        return UCN_ERR_MALFORMED;
    }
    if (frame->destination != node->config.node_id) {
        return UCN_OK;
    }
    {
        const ucn_candidate_route_t *candidate =
            find_candidate_route(node, frame->source, candidate_id);

        if (candidate == NULL || candidate->wire_profile != frame->wire_profile) {
            return UCN_ERR_NOT_FOUND;
        }
    }
    egress_link = find_candidate_link(node, frame->source, candidate_id);
    if (egress_link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    return send_control_on_link_profile(
        node, egress_link, frame->source, UCN_MSG_PATH_PROBE_ACK,
        frame->payload, frame->payload_length, frame->wire_profile);
}

/*
 * EN: Validates and processes `handle_path_probe_ack` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_path_probe_ack`。
 */
static ucn_result_t handle_path_probe_ack(ucn_node_t *node,
                                          const ucn_frame_t *frame)
{
    uint32_t candidate_id;
    ucn_candidate_route_t *candidate;

    if (frame->payload_length != UCN_PATH_PROBE_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    candidate_id = read_u32_be(frame->payload);
    if (candidate_id == 0U || read_u32_be(frame->payload + 4U) == 0U) {
        return UCN_ERR_MALFORMED;
    }
    if (frame->destination != node->config.node_id) {
        return UCN_OK;
    }
    candidate = find_candidate_route(node, frame->source, candidate_id);
    if (candidate == NULL || !candidate->originated_here ||
        candidate->wire_profile != frame->wire_profile) {
        return UCN_ERR_NOT_FOUND;
    }
    if (candidate->probes_acked < UCN_PATH_PROBE_REQUIRED_ACKS) {
        const uint32_t sampled_rtt =
            node->now_ms - read_u32_be(frame->payload + 8U);
        const uint16_t bounded_rtt = sampled_rtt > UINT16_MAX ?
                                         UINT16_MAX : (uint16_t)sampled_rtt;

        if (!candidate->verified_rtt_valid) {
            candidate->verified_rtt_ms = bounded_rtt;
            candidate->verified_rtt_valid = true;
        } else {
            const uint32_t weighted =
                (uint32_t)candidate->verified_rtt_ms *
                    (100U - UCN_POLICY_QUALITY_EWMA_ALPHA_PERCENT) +
                (uint32_t)bounded_rtt * UCN_POLICY_QUALITY_EWMA_ALPHA_PERCENT;

            candidate->verified_rtt_ms = (uint16_t)((weighted + 50U) / 100U);
        }
        candidate->probes_acked++;
        candidate->expires_at_ms =
            ucn_deadline_from_now(node->now_ms,
                                  UCN_ROUTE_CANDIDATE_TIMEOUT_MS);
        node->stats.path_probe_acks_received++;
    }
    return UCN_OK;
}

/*
 * EN: Validates and processes `handle_path_activate` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_path_activate`。
 */
static ucn_result_t handle_path_activate(ucn_node_t *node,
                                         const ucn_frame_t *frame)
{
    uint32_t candidate_id;
    uint16_t route_epoch;
    ucn_link_t *egress_link;
    ucn_result_t result;

    if (frame->payload_length != UCN_PATH_ACTIVATE_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    candidate_id = read_u32_be(frame->payload);
    route_epoch = read_u16_be(frame->payload + 4U);
    if (candidate_id == 0U || route_epoch == 0U) {
        return UCN_ERR_MALFORMED;
    }
    if (frame->destination == node->config.node_id) {
        result = activate_candidate_route(node, frame->source, candidate_id,
                                          route_epoch, frame->wire_profile);
        if (result != UCN_OK) {
            return result;
        }
        egress_link = find_link(node, frame->source);
        if (egress_link == NULL) {
            return UCN_ERR_NOT_FOUND;
        }
        return send_control_on_link_profile(
            node, egress_link, frame->source, UCN_MSG_PATH_ACTIVATE_ACK,
            frame->payload, frame->payload_length, frame->wire_profile);
    }

    egress_link = find_candidate_link(node, frame->destination, candidate_id);
    if (egress_link == NULL || frame->hop_limit <= 1U) {
        return egress_link == NULL ? UCN_ERR_NOT_FOUND : UCN_ERR_TTL;
    }
    result = activate_candidate_route(node, frame->source, candidate_id,
                                      route_epoch, frame->wire_profile);
    if (result != UCN_OK) {
        return result;
    }
    {
        ucn_frame_t forwarded = *frame;

        --forwarded.hop_limit;
        result = send_frame_on_link(node, egress_link, &forwarded);
    }
    if (result != UCN_OK) {
        return result;
    }
    return activate_candidate_route(node, frame->destination, candidate_id,
                                    route_epoch, frame->wire_profile);
}

/*
 * EN: Validates and processes `handle_path_activate_ack` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_path_activate_ack`。
 */
static ucn_result_t handle_path_activate_ack(ucn_node_t *node,
                                             const ucn_frame_t *frame)
{
    uint32_t candidate_id;
    uint16_t route_epoch;
    ucn_result_t result;

    if (frame->payload_length != UCN_PATH_ACTIVATE_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    candidate_id = read_u32_be(frame->payload);
    route_epoch = read_u16_be(frame->payload + 4U);
    if (candidate_id == 0U || route_epoch == 0U ||
        frame->destination != node->config.node_id) {
        return UCN_ERR_MALFORMED;
    }
    result = activate_candidate_route(node, frame->source, candidate_id,
                                      route_epoch, frame->wire_profile);
    if (result == UCN_OK) {
        node->stats.route_switches++;
    }
    return result;
}
#endif

/*
 * EN: Forwards or delivers `forward_route_request` through the bounded Lite/Full Node path.
 * 中文：通过有界的 Lite/Full Node 路径转发或投递 `forward_route_request`。
 */
static ucn_result_t forward_route_request(ucn_node_t *node,
                                          ucn_link_t *ingress_link,
                                          const ucn_frame_t *frame)
{
    ucn_frame_t forwarded = *frame;
    ucn_route_cost_t route_cost;
    uint8_t hop_count;
    size_t index;
    size_t sent_count = 0U;
    ucn_result_t last_error = UCN_ERR_NOT_FOUND;

    if ((ingress_link == NULL && frame->hop_limit == 0U) ||
        (ingress_link != NULL && frame->hop_limit <= 1U)) {
        return UCN_ERR_TTL;
    }
    if (frame->payload_length != route_request_payload_size(
                                     frame->wire_profile)) {
        return UCN_ERR_MALFORMED;
    }
    route_cost = read_route_cost_for_profile(
        frame->payload + route_request_cost_offset(frame), frame->wire_profile);
    hop_count = frame->payload[route_request_hop_offset(frame)];
    if (hop_count == UINT8_MAX) {
        return UCN_ERR_TTL;
    }
    /* The origin has not consumed a Link hop yet.  Relays decrement once
     * before forwarding, so an expanding-ring value of 2 reaches exactly two
     * Links (A-B-C) rather than counting the local flood operation as a hop. */
    if (ingress_link != NULL) {
        --forwarded.hop_limit;
    }

    for (index = 0U; index < node->link_count; ++index) {
        uint8_t payload[UCN_ROUTE_REQ_MAX_PAYLOAD_BYTES];
        ucn_route_cost_t next_cost;
        ucn_result_t result;

        if (node->links[index] == ingress_link) {
            continue;
        }
#if UCN_FEATURE_CANDIDATE_ROUTING
        if ((frame->payload[route_request_flags_offset(frame)] &
             UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U &&
            !link_is_candidate_eligible(node, node->links[index])) {
            continue;
        }
#endif
        (void)memcpy(payload, frame->payload, frame->payload_length);
        result = accumulate_route_cost(route_cost,
                                       link_route_cost(node->links[index]),
                                       &next_cost);
        if (result != UCN_OK) {
            last_error = result;
            continue;
        }
        result = write_route_cost_for_profile(
            payload + route_request_cost_offset(frame), frame->wire_profile,
            next_cost);
        if (result != UCN_OK) {
            last_error = result;
            continue;
        }
        payload[route_request_hop_offset(frame)] = (uint8_t)(hop_count + 1U);
        forwarded.payload = payload;
        result = send_frame_on_link(node, node->links[index], &forwarded);
        if (result == UCN_OK) {
            ++sent_count;
        } else {
            last_error = result;
        }
    }

    return sent_count != 0U ? UCN_OK : last_error;
}

/*
 * EN: Validates and submits `send_route_error` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_route_error` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_route_error(ucn_node_t *node,
                                     ucn_link_t *upstream_link,
                                     ucn_node_id_t origin,
                                     ucn_node_id_t unreachable,
                                     ucn_wire_profile_t wire_profile)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(wire_profile);
    uint8_t payload[UCN_ROUTE_ERROR_MAX_PAYLOAD_BYTES];
    size_t payload_length = route_error_payload_size(wire_profile, false);
    ucn_result_t result;

    if (descriptor == NULL || payload_length > sizeof(payload) ||
        upstream_link == NULL || origin == 0U ||
        origin == UCN_NODE_BROADCAST) {
        return UCN_ERR_ARGUMENT;
    }

    write_uint_be(payload, descriptor->address_bytes, unreachable);
    result = send_control_on_link_profile(
        node, upstream_link, origin, UCN_MSG_ROUTE_ERROR, payload,
        (uint16_t)payload_length, wire_profile);
    if (result == UCN_OK) {
        node->stats.route_errors_sent++;
    }
    return result;
}

#if UCN_FEATURE_PATH
/*
 * EN: Validates and submits `send_path_route_error` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_path_route_error` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_path_route_error(ucn_node_t *node,
                                          ucn_link_t *upstream_link,
                                          ucn_node_id_t origin,
                                          ucn_node_id_t unreachable,
                                          ucn_session_id_t owner_session_id,
                                          ucn_path_id_t path_id,
                                          ucn_wire_profile_t wire_profile)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(wire_profile);
    uint8_t payload[UCN_ROUTE_ERROR_MAX_PAYLOAD_BYTES];
    size_t payload_length = route_error_payload_size(wire_profile, true);
    size_t offset;
    ucn_result_t result;

    if (descriptor == NULL || payload_length > sizeof(payload) ||
        upstream_link == NULL || origin == 0U || origin == UCN_NODE_BROADCAST ||
        unreachable == 0U || unreachable == UCN_NODE_BROADCAST ||
        owner_session_id == 0U || path_id == 0U) {
        return UCN_ERR_ARGUMENT;
    }

    offset = 0U;
    write_uint_be(payload + offset, descriptor->address_bytes, unreachable);
    offset += descriptor->address_bytes;
    write_uint_be(payload + offset, descriptor->address_bytes,
                  owner_session_id);
    offset += descriptor->address_bytes;
    write_uint_be(payload + offset, descriptor->path_id_bytes, path_id);
    result = send_control_on_link_profile(
        node, upstream_link, origin, UCN_MSG_ROUTE_ERROR, payload,
        (uint16_t)payload_length, wire_profile);
    if (result == UCN_OK) {
        node->stats.route_errors_sent++;
        node->stats.path_route_errors_sent++;
    }
    return result;
}
#endif

/*
 * EN: Clears or releases `invalidate_route_to` from bounded Lite/Full Node state.
 * 中文：从固定容量的 Lite/Full Node 状态中清除或释放 `invalidate_route_to`。
 */
static void invalidate_route_to(ucn_node_t *node, ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid && !node->routes[index].is_static &&
            node->routes[index].destination == destination) {
            node->routes[index].valid = false;
        }
    }
#if UCN_FEATURE_CANDIDATE_ROUTING
    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        if (node->candidates[index].valid &&
            node->candidates[index].destination == destination) {
            node->candidates[index].valid = false;
        }
    }
#endif
    clear_discovery(node, destination);
}

/*
 * EN: Validates `route_request_frame` before Lite/Full Node state is used or changed.
 * 中文：在使用或修改 Lite/Full Node 状态前验证 `route_request_frame`。
 */
static ucn_result_t validate_route_request_frame(ucn_node_t *node,
                                                 ucn_link_t *ingress_link,
                                                 const ucn_frame_t *frame)
{
    ucn_node_id_t target;
    uint32_t request_id;
    bool is_candidate;

    if (frame->payload_length != route_request_payload_size(
                                     frame->wire_profile)) {
        return UCN_ERR_MALFORMED;
    }
    target = route_request_target(frame);
    request_id = read_u32_be(frame->payload + route_request_id_offset(frame));
    is_candidate =
        (frame->payload[route_request_flags_offset(frame)] &
         UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U;
    if (frame->source == 0U || frame->source == UCN_NODE_BROADCAST ||
        target == 0U || target == UCN_NODE_BROADCAST || request_id == 0U ||
        (frame->payload[route_request_flags_offset(frame)] &
         (uint8_t)~UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U) {
        return UCN_ERR_MALFORMED;
    }
#if !UCN_FEATURE_CANDIDATE_ROUTING
    (void)node;
    (void)ingress_link;
    if (is_candidate) {
        return UCN_ERR_CONFIG;
    }
#else
    if (is_candidate && !link_is_candidate_eligible(node, ingress_link)) {
        return UCN_ERR_ACCESS;
    }
#endif
    return UCN_OK;
}

/*
 * EN: Validates `inbound_hop_scope` before Lite/Full Node state is used or changed.
 * 中文：在使用或修改 Lite/Full Node 状态前验证 `inbound_hop_scope`。
 */
static ucn_result_t validate_inbound_hop_scope(ucn_node_t *node,
                                               const ucn_frame_t *frame)
{
    const uint8_t maximum = node->config.default_hop_limit;

    if (frame->hop_limit > maximum) {
        node->stats.hop_scope_rejected++;
        return UCN_ERR_TTL;
    }
    if (frame->message_type == UCN_MSG_ROUTE_REQ &&
        frame->payload_length == route_request_payload_size(
                                     frame->wire_profile)) {
        const uint8_t travelled =
            frame->payload[route_request_hop_offset(frame)];
        const uint16_t original_ring_scope =
            (uint16_t)travelled + (uint16_t)frame->hop_limit - UINT16_C(1);

        if (travelled > maximum || original_ring_scope > maximum) {
            node->stats.hop_scope_rejected++;
            return UCN_ERR_TTL;
        }
    }
    if (frame->message_type == UCN_MSG_ROUTE_REPLY &&
        frame->payload_length == route_reply_payload_size(
                                     frame->wire_profile) &&
        frame->payload[route_reply_hop_offset(frame)] >= maximum) {
        node->stats.hop_scope_rejected++;
        return UCN_ERR_TTL;
    }
#if UCN_FEATURE_PATH
    if (frame->message_type == UCN_MSG_PATH_INSTALL) {
        const ucn_wire_profile_descriptor_t *descriptor =
            ucn_wire_profile_get_descriptor(frame->wire_profile);

        if (descriptor != NULL &&
            path_install_payload_length_supported(frame->wire_profile,
                                                   frame->payload_length) &&
            frame->payload[path_install_remaining_hops_offset(descriptor)] >
                maximum) {
            node->stats.hop_scope_rejected++;
            return UCN_ERR_TTL;
        }
    }
#endif
    return UCN_OK;
}

/*
 * EN: Validates and processes `handle_route_request` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_route_request`。
 */
static ucn_result_t handle_route_request(ucn_node_t *node,
                                         ucn_link_t *ingress_link,
                                         const ucn_frame_t *frame)
{
    ucn_node_id_t origin;
    ucn_node_id_t target;
    uint32_t request_id;
    ucn_route_cost_t route_cost;
    uint8_t hop_count;
    bool is_candidate;
    ucn_result_t result;

    result = validate_route_request_frame(node, ingress_link, frame);
    if (result != UCN_OK) {
        return result;
    }
    origin = frame->source;
    target = route_request_target(frame);
    request_id = read_u32_be(frame->payload + route_request_id_offset(frame));
    route_cost = read_route_cost_for_profile(
        frame->payload + route_request_cost_offset(frame), frame->wire_profile);
    hop_count = frame->payload[route_request_hop_offset(frame)];
    is_candidate =
        (frame->payload[route_request_flags_offset(frame)] &
         UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U;

#if UCN_FEATURE_CANDIDATE_ROUTING
    result = is_candidate ?
             learn_candidate_route(node, origin, request_id, ingress_link,
                                   route_cost, hop_count, frame->wire_profile,
                                   false) :
             learn_route(node, origin, ingress_link, route_cost, hop_count,
                         route_epoch_from_request_id(frame->wire_profile,
                                                     request_id));
#else
    result = learn_route(node, origin, ingress_link, route_cost, hop_count,
                         route_epoch_from_request_id(frame->wire_profile,
                                                     request_id));
#endif
    if (result != UCN_OK) {
        return result;
    }

    if (target == node->config.node_id) {
        const ucn_wire_profile_descriptor_t *descriptor =
            ucn_wire_profile_get_descriptor(frame->wire_profile);
        uint8_t reply[UCN_ROUTE_REPLY_MAX_PAYLOAD_BYTES];
        size_t reply_length = route_reply_payload_size(frame->wire_profile);
        ucn_route_entry_t *reverse_route = find_active_route(node, origin);

        if (!is_candidate && (reverse_route == NULL ||
                              reverse_route->route_epoch == 0U)) {
            return UCN_ERR_NOT_FOUND;
        }

        if (descriptor == NULL || reply_length > sizeof(reply)) {
            return UCN_ERR_CONFIG;
        }
        write_u32_be(reply, request_id);
        /* A ROUTE_REPLY advertises distance from the current reply sender to
         * the target, not the completed RREQ's origin-to-target distance.
         * Starting at zero lets every return hop add its own outbound Link
         * Cost/Hop.  Stored route metrics then strictly decrease toward the
         * target, which is the bounded AODV-Lite loop-freedom invariant. */
        result = write_route_cost_for_profile(
            reply + route_reply_cost_offset(), frame->wire_profile, 0U);
        if (result != UCN_OK) {
            return result;
        }
        reply[route_reply_hop_offset(frame)] = 0U;
        reply[route_reply_flags_offset(frame)] =
            is_candidate ? UCN_ROUTE_REQ_FLAG_CANDIDATE : 0U;
        write_uint_be(reply + route_reply_epoch_offset(frame),
                      descriptor->route_epoch_bytes,
                      is_candidate ? 0U : reverse_route->route_epoch);
        result = send_control_on_link_profile(
            node, ingress_link, origin, UCN_MSG_ROUTE_REPLY, reply,
            (uint16_t)reply_length, frame->wire_profile);
        if (result == UCN_OK) {
            node->stats.route_replies_sent++;
        }
        return result;
    }

    return forward_route_request(node, ingress_link, frame);
}

/*
 * EN: Validates and processes `handle_route_reply` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_route_reply`。
 */
static ucn_result_t handle_route_reply(ucn_node_t *node,
                                       ucn_link_t *ingress_link,
                                       const ucn_frame_t *frame,
                                       bool *consumed)
{
    ucn_node_id_t origin;
    ucn_node_id_t target;
    const ucn_wire_profile_descriptor_t *descriptor;
    uint32_t request_id;
    ucn_route_cost_t route_cost;
    uint16_t route_epoch;
    uint8_t hop_count;
    bool is_candidate;
    size_t index;
    ucn_result_t result;

    *consumed = false;
    descriptor = ucn_wire_profile_get_descriptor(frame->wire_profile);
    if (descriptor == NULL ||
        frame->payload_length != route_reply_payload_size(frame->wire_profile)) {
        return UCN_ERR_MALFORMED;
    }
    origin = frame->destination;
    target = frame->source;
    request_id = read_u32_be(frame->payload);
    route_cost = read_route_cost_for_profile(
        frame->payload + route_reply_cost_offset(), frame->wire_profile);
    hop_count = frame->payload[route_reply_hop_offset(frame)];
    is_candidate =
        (frame->payload[route_reply_flags_offset(frame)] &
         UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U;
    route_epoch = (uint16_t)read_uint_be(
        frame->payload + route_reply_epoch_offset(frame),
        descriptor->route_epoch_bytes);
    if (origin == 0U || origin == UCN_NODE_BROADCAST || request_id == 0U ||
        (frame->payload[route_reply_flags_offset(frame)] &
         (uint8_t)~UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U ||
        (!is_candidate && route_epoch == 0U)) {
        return UCN_ERR_MALFORMED;
    }
    if (hop_count == UINT8_MAX) {
        return UCN_ERR_TTL;
    }
    result = accumulate_route_cost(route_cost, link_route_cost(ingress_link),
                                   &route_cost);
    if (result != UCN_OK) {
        return result;
    }
    hop_count++;
#if !UCN_FEATURE_CANDIDATE_ROUTING
    if (is_candidate) {
        return UCN_ERR_CONFIG;
    }
#else
    if (is_candidate && !link_is_candidate_eligible(node, ingress_link)) {
        return UCN_ERR_ACCESS;
    }

    if (is_candidate && frame->destination == node->config.node_id) {
        ucn_route_entry_t *active_route = find_active_route(node, target);
        uint16_t candidate_link_cost;
        bool candidate_link_cost_known;
        bool candidate_link_selectable;
        bool verification_requested = false;

        for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
            if (node->discoveries[index].active &&
                node->discoveries[index].destination == target &&
                node->discoveries[index].request_id == request_id &&
                node->discoveries[index].is_candidate) {
                verification_requested =
                    node->discoveries[index].require_verified_rtt;
                break;
            }
        }

        candidate_link_selectable = link_local_select_cost(
            node, ingress_link, &candidate_link_cost_known,
            &candidate_link_cost);
        (void)candidate_link_cost_known;
        (void)candidate_link_cost;
        if (!candidate_link_selectable || active_route == NULL ||
            active_route->is_static ||
            (!candidate_route_is_locally_better(node, active_route, route_cost,
                                                ingress_link) &&
             !(verification_requested && !active_route->verified_rtt_valid &&
               route_cost <= active_route->route_cost &&
               hop_count <= active_route->hop_count))) {
            for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
                if (node->discoveries[index].active &&
                    node->discoveries[index].destination == target &&
                    node->discoveries[index].request_id == request_id &&
                    node->discoveries[index].is_candidate) {
                    node->discoveries[index].active = false;
                    break;
                }
            }
            node->stats.candidate_rejected++;
            *consumed = true;
            return UCN_OK;
        }
        result = learn_candidate_route(node, target, request_id, ingress_link,
                                       route_cost, hop_count,
                                       frame->wire_profile, true);
        if (result != UCN_OK) {
            return result;
        }
        for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
            if (node->discoveries[index].active &&
                node->discoveries[index].destination == target &&
                node->discoveries[index].request_id == request_id &&
                node->discoveries[index].is_candidate) {
                node->discoveries[index].active = false;
                break;
            }
        }
        *consumed = true;
        return UCN_OK;
    }
#endif

#if UCN_FEATURE_CANDIDATE_ROUTING
    result = is_candidate ?
             learn_candidate_route(node, target, request_id, ingress_link,
                                   route_cost, hop_count, frame->wire_profile,
                                   false) :
             learn_route(node, target, ingress_link, route_cost, hop_count,
                         route_epoch);
#else
    result = learn_route(node, target, ingress_link, route_cost, hop_count,
                         route_epoch);
#endif
    if (result != UCN_OK) {
        return result;
    }

    if (frame->destination != node->config.node_id) {
        return UCN_OK;
    }

    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        if (node->discoveries[index].active &&
            node->discoveries[index].destination == target &&
            node->discoveries[index].request_id == request_id) {
            node->discoveries[index].active = false;
            *consumed = true;
            return UCN_OK;
        }
    }

    *consumed = true;
    return UCN_OK;
}

/*
 * EN: Validates and processes `handle_route_error` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_route_error`。
 */
static ucn_result_t handle_route_error(ucn_node_t *node,
                                       const ucn_frame_t *frame,
                                       bool *consumed)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(frame->wire_profile);
    size_t normal_length = route_error_payload_size(frame->wire_profile, false);
    size_t path_length = route_error_payload_size(frame->wire_profile, true);
    ucn_node_id_t unreachable;
    bool path_scoped = false;

    *consumed = false;
    if (descriptor == NULL ||
        (frame->payload_length != normal_length &&
         frame->payload_length != path_length)) {
        return UCN_ERR_MALFORMED;
    }
    unreachable = read_uint_be(frame->payload, descriptor->address_bytes);
    if (unreachable == 0U || unreachable == UCN_NODE_BROADCAST) {
        return UCN_ERR_MALFORMED;
    }
    path_scoped = frame->payload_length == path_length;
    if (path_scoped) {
#if UCN_FEATURE_PATH
        const size_t session_offset = descriptor->address_bytes;
        const size_t path_offset = session_offset + descriptor->address_bytes;
        const ucn_node_id_t owner = frame->destination;
        const ucn_session_id_t owner_session_id = read_uint_be(
            frame->payload + session_offset, descriptor->address_bytes);
        const ucn_path_id_t path_id = read_uint_be(
            frame->payload + path_offset, descriptor->path_id_bytes);

        if (owner == 0U || owner == UCN_NODE_BROADCAST ||
            owner_session_id == 0U || path_id == 0U) {
            return UCN_ERR_MALFORMED;
        }
        revoke_path_and_mark_local_policy(node, owner, owner_session_id,
                                           path_id, unreachable);
#else
        return UCN_ERR_CONFIG;
#endif
    } else {
        invalidate_route_to(node, unreachable);
    }
    if (frame->destination == node->config.node_id) {
        *consumed = true;
    }
    return UCN_OK;
}

#if UCN_FEATURE_PATH
/*
 * EN: Verifies whether `authorize_path_control` is authorized by the Lite/Full Node contract.
 * 中文：验证 `authorize_path_control` 是否获得 Lite/Full Node 合同授权。
 */
static ucn_result_t authorize_path_control(ucn_node_t *node,
                                           ucn_link_t *ingress_link,
                                           const ucn_frame_t *frame,
                                           ucn_path_control_operation_t operation,
                                           ucn_path_id_t path_id,
                                           ucn_node_id_t destination,
                                           ucn_node_id_t next_hop)
{
    if (node->security_ops == NULL || node->path_control_authorize == NULL) {
        return UCN_ERR_ACCESS;
    }
    return node->path_control_authorize(node->path_control_authorize_context,
                                        ingress_link, frame, operation, path_id,
                                        destination, next_hop);
}

typedef enum path_control_budget_take_result {
    PATH_CONTROL_BUDGET_TAKEN = 0,
    PATH_CONTROL_BUDGET_RATE_LIMITED = 1,
    PATH_CONTROL_BUDGET_SOURCE_FULL = 2
} path_control_budget_take_result_t;

/*
 * EN: Records `note_path_control_authorization_rejected` in bounded Lite/Full Node state or statistics.
 * 中文：在固定容量的 Lite/Full Node 状态或统计中记录 `note_path_control_authorization_rejected`。
 */
static void note_path_control_authorization_rejected(
    ucn_node_t *node,
    uint8_t message_type)
{
    if (message_type == UCN_MSG_PATH_INSTALL) {
        node->stats.path_install_authorization_rejected++;
    } else if (message_type == UCN_MSG_PATH_REVOKE) {
        node->stats.path_revoke_authorization_rejected++;
    }
}

/*
 * EN: Initializes `initialize_path_control_source_budget` for Lite/Full Node using caller-owned fixed storage.
 * 中文：使用调用方提供的固定存储初始化 Lite/Full Node 的 `initialize_path_control_source_budget`。
 */
static void initialize_path_control_source_budget(
    ucn_path_control_source_budget_t *budget,
    ucn_node_id_t source,
    ucn_session_id_t session_id,
    uint32_t now_ms)
{
    size_t index;

    (void)memset(budget, 0, sizeof(*budget));
    budget->occupied = true;
    budget->source = source;
    budget->session_id = session_id;
    budget->last_seen_ms = now_ms;
    for (index = 0U; index < UCN_PATH_CONTROL_OPERATION_COUNT; ++index) {
        budget->tokens[index] = UCN_PATH_CONTROL_RX_TOKEN_BURST;
        budget->last_refill_ms[index] = now_ms;
    }
}

/*
 * EN: Searches bounded Lite/Full Node state for `path_control_source_budget`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `path_control_source_budget`。
 */
static ucn_path_control_source_budget_t *find_path_control_source_budget(
    ucn_node_t *node,
    ucn_node_id_t source,
    ucn_session_id_t session_id)
{
    ucn_path_control_source_budget_t *same_source = NULL;
    ucn_path_control_source_budget_t *free_slot = NULL;
    size_t index;

    /* An inactive authenticated source cannot occupy one of the fixed slots
     * forever.  Reclamation is lazy and therefore adds no periodic scan. */
    for (index = 0U; index < UCN_PATH_CONTROL_RX_SOURCE_DEPTH; ++index) {
        ucn_path_control_source_budget_t *slot =
            &node->path_control_source_budgets[index];

        if (slot->occupied &&
            ucn_elapsed_at_least(node->now_ms, slot->last_seen_ms,
                                 UCN_PATH_CONTROL_RX_SOURCE_IDLE_MS)) {
            (void)memset(slot, 0, sizeof(*slot));
            node->stats.path_control_budget_sources_reclaimed++;
        }
    }

    for (index = 0U; index < UCN_PATH_CONTROL_RX_SOURCE_DEPTH; ++index) {
        ucn_path_control_source_budget_t *slot =
            &node->path_control_source_budgets[index];

        if (slot->occupied && slot->source == source &&
            slot->session_id == session_id) {
            return slot;
        }
        if (slot->occupied && slot->source == source) {
            same_source = slot;
        } else if (!slot->occupied && free_slot == NULL) {
            free_slot = slot;
        }
    }

    /* Security and the product authorizer have already accepted this frame.
     * A new authenticated Session therefore replaces the old generation for
     * the same source instead of leaking one fixed slot per key rotation. */
    if (same_source != NULL) {
        initialize_path_control_source_budget(same_source, source, session_id,
                                              node->now_ms);
        node->stats.path_control_budget_session_rotations++;
        return same_source;
    }
    if (free_slot != NULL) {
        initialize_path_control_source_budget(free_slot, source, session_id,
                                              node->now_ms);
    }
    return free_slot;
}

/*
 * EN: Removes and returns `take_path_control_source_token` from a bounded Lite/Full Node queue or slot.
 * 中文：从固定容量的 Lite/Full Node 队列或槽位中移除并返回 `take_path_control_source_token`。
 */
static path_control_budget_take_result_t take_path_control_source_token(
    ucn_node_t *node,
    const ucn_frame_t *frame,
    ucn_path_control_operation_t operation)
{
    ucn_path_control_source_budget_t *source_budget;
    uint8_t *tokens;
    uint32_t *last_refill_ms;
    uint32_t elapsed;
    uint32_t refill_count;

    source_budget = find_path_control_source_budget(node, frame->source,
                                                    frame->session_id);
    if (source_budget == NULL) {
        return PATH_CONTROL_BUDGET_SOURCE_FULL;
    }

    tokens = &source_budget->tokens[operation];
    last_refill_ms = &source_budget->last_refill_ms[operation];
    elapsed = node->now_ms - *last_refill_ms;
    refill_count = elapsed / UCN_PATH_CONTROL_RX_TOKEN_REFILL_MS;
    if (refill_count != 0U) {
        const uint32_t missing =
            (uint32_t)UCN_PATH_CONTROL_RX_TOKEN_BURST - (uint32_t)*tokens;

        if (refill_count >= missing) {
            *tokens = UCN_PATH_CONTROL_RX_TOKEN_BURST;
        } else {
            *tokens = (uint8_t)((uint32_t)*tokens + refill_count);
        }
        *last_refill_ms += refill_count * UCN_PATH_CONTROL_RX_TOKEN_REFILL_MS;
    }
    source_budget->last_seen_ms = node->now_ms;
    if (*tokens == 0U) {
        return PATH_CONTROL_BUDGET_RATE_LIMITED;
    }
    --(*tokens);
    return PATH_CONTROL_BUDGET_TAKEN;
}

/*
 * EN: Validates and processes `handle_path_install` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_path_install`。
 */
static ucn_result_t handle_path_install(ucn_node_t *node,
                                        ucn_link_t *ingress_link,
                                        const ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(frame->wire_profile);
    ucn_path_id_t path_id;
    ucn_node_id_t destination;
    ucn_node_id_t next_hop;
    uint32_t lease_ms;
    uint8_t remaining_hops;
    bool has_capability;
    ucn_path_capability_t capability;
    const ucn_path_capability_t *capability_ptr;
    ucn_result_t result;

    if (descriptor == NULL ||
        !path_install_payload_length_supported(frame->wire_profile,
                                               frame->payload_length) ||
        frame->destination != node->config.node_id || frame->source == 0U ||
        frame->source == UCN_NODE_BROADCAST || frame->session_id == 0U) {
        return UCN_ERR_MALFORMED;
    }
    path_id = read_uint_be(frame->payload, descriptor->path_id_bytes);
    destination = read_uint_be(
        frame->payload + path_install_destination_offset(descriptor),
        descriptor->address_bytes);
    next_hop = read_uint_be(
        frame->payload + path_install_next_hop_offset(descriptor),
        descriptor->address_bytes);
    lease_ms = read_u32_be(
        frame->payload + path_install_lease_offset(descriptor));
    remaining_hops =
        frame->payload[path_install_remaining_hops_offset(descriptor)];
    has_capability = (size_t)frame->payload_length ==
        path_install_capable_payload_size(frame->wire_profile);
    if (!has_capability) {
        capability_ptr = NULL;
    } else {
        capability.maximum_wire_profile = (ucn_wire_profile_t)
            frame->payload[path_install_maximum_profile_offset(descriptor)];
        capability.minimum_mtu = read_u16_be(
            frame->payload + path_install_minimum_mtu_offset(descriptor));
        if (capability.minimum_mtu == 0U) {
            if (capability.maximum_wire_profile !=
                UCN_WIRE_PROFILE_UNSPECIFIED) {
                return UCN_ERR_MALFORMED;
            }
            capability_ptr = NULL;
        } else {
            if (ucn_wire_profile_get_descriptor(
                    capability.maximum_wire_profile) == NULL) {
                return UCN_ERR_MALFORMED;
            }
            capability_ptr = &capability;
        }
    }
    if (path_id == 0U || destination == 0U || destination == UCN_NODE_BROADCAST ||
        next_hop == UCN_NODE_BROADCAST ||
        remaining_hops > UCN_MAX_HOPS ||
        remaining_hops > node->config.default_hop_limit ||
        remaining_hops > descriptor->max_hops ||
        (next_hop == 0U && remaining_hops != 0U) ||
        (next_hop != 0U && remaining_hops == 0U) ||
        !ucn_duration_is_valid(lease_ms) ||
        (next_hop == 0U && destination != node->config.node_id)) {
        return UCN_ERR_MALFORMED;
    }
    result = validate_path_wire_scope(frame->wire_profile, path_id,
                                      destination, next_hop);
    if (result != UCN_OK) {
        return result;
    }
    result = authorize_path_control(node, ingress_link, frame,
                                    UCN_PATH_CONTROL_INSTALL, path_id,
                                    destination, next_hop);
    if (result != UCN_OK) {
        node->stats.path_install_authorization_rejected++;
        return result;
    }
    {
        const path_control_budget_take_result_t budget_result =
            take_path_control_source_token(node, frame,
                                           UCN_PATH_CONTROL_INSTALL);

        if (budget_result == PATH_CONTROL_BUDGET_SOURCE_FULL) {
            node->stats.path_control_budget_source_full++;
            return UCN_ERR_NO_SPACE;
        }
        if (budget_result == PATH_CONTROL_BUDGET_RATE_LIMITED) {
            node->stats.path_install_budget_rejected++;
            return UCN_ERR_NO_SPACE;
        }
    }
    result = install_path_forward_entry(node, frame->source, frame->session_id,
                                        path_id, destination, next_hop,
                                        remaining_hops, lease_ms,
                                        capability_ptr);
    if (result == UCN_OK) {
        node->stats.path_installs_received++;
    } else if (result == UCN_ERR_NO_SPACE) {
        node->stats.path_install_table_full++;
    }
    return result;
}

/*
 * EN: Validates and processes `handle_path_revoke` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_path_revoke`。
 */
static ucn_result_t handle_path_revoke(ucn_node_t *node,
                                       ucn_link_t *ingress_link,
                                       const ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(frame->wire_profile);
    ucn_path_id_t path_id;
    ucn_node_id_t destination;
    ucn_result_t result;

    if (descriptor == NULL ||
        frame->payload_length != path_revoke_payload_size(frame->wire_profile) ||
        frame->destination != node->config.node_id || frame->source == 0U ||
        frame->source == UCN_NODE_BROADCAST || frame->session_id == 0U) {
        return UCN_ERR_MALFORMED;
    }
    path_id = read_uint_be(frame->payload, descriptor->path_id_bytes);
    destination = read_uint_be(frame->payload + descriptor->path_id_bytes,
                               descriptor->address_bytes);
    if (path_id == 0U || destination == 0U || destination == UCN_NODE_BROADCAST) {
        return UCN_ERR_MALFORMED;
    }
    result = authorize_path_control(node, ingress_link, frame,
                                    UCN_PATH_CONTROL_REVOKE, path_id,
                                    destination, 0U);
    if (result != UCN_OK) {
        node->stats.path_revoke_authorization_rejected++;
        return result;
    }
    {
        const path_control_budget_take_result_t budget_result =
            take_path_control_source_token(node, frame,
                                           UCN_PATH_CONTROL_REVOKE);

        if (budget_result == PATH_CONTROL_BUDGET_SOURCE_FULL) {
            node->stats.path_control_budget_source_full++;
            return UCN_ERR_NO_SPACE;
        }
        if (budget_result == PATH_CONTROL_BUDGET_RATE_LIMITED) {
            node->stats.path_revoke_budget_rejected++;
            return UCN_ERR_NO_SPACE;
        }
    }
    result = ucn_path_revoke(&node->path_state, frame->source, frame->session_id,
                             path_id, destination);
    if (result != UCN_OK && result != UCN_ERR_NOT_FOUND) {
        return result;
    }
    node->stats.path_revokes_received++;
    return UCN_OK;
}
#endif

/* HELLO is strictly link-local: it binds an ingress Link to one Node ID and
 * then hands the candidate to the configured join policy.  It is never
 * delivered to the application or forwarded by the mesh router. */
/*
 * EN: Validates and processes `handle_hello` in the Lite/Full Node receive path.
 * 中文：在 Lite/Full Node 接收路径中验证并处理 `handle_hello`。
 */
static ucn_result_t handle_hello(ucn_node_t *node,
                                 ucn_link_t *ingress_link,
                                 const ucn_frame_t *frame)
{
    ucn_node_id_t peer_node_id;
    ucn_wire_profile_t peer_receive_profile;

    if (frame->payload_length != UCN_HELLO_PAYLOAD_BYTES ||
        (frame->destination != node->config.node_id &&
         frame->destination != UCN_NODE_BROADCAST)) {
        return UCN_ERR_MALFORMED;
    }
    peer_receive_profile = frame->payload[0];
    if (ucn_wire_profile_get_descriptor(peer_receive_profile) == NULL ||
        peer_receive_profile < frame->wire_profile) {
        return UCN_ERR_MALFORMED;
    }

    peer_node_id = frame->source;
    if (frame->source == 0U || frame->source == UCN_NODE_BROADCAST ||
        peer_node_id == node->config.node_id ||
        (ingress_link->peer_node_id != 0U &&
         ingress_link->peer_node_id != peer_node_id)) {
        return UCN_ERR_MALFORMED;
    }

    ingress_link->peer_node_id = peer_node_id;
    {
        const ucn_result_t result =
            ucn_node_observe_neighbor(node, ingress_link, node->now_ms);

        if (result != UCN_OK) {
            return result;
        }
    }
    return ucn_node_set_link_wire_profile_limit(node, ingress_link,
                                                 peer_receive_profile);
}

/*
 * EN: Initializes a Node instance from validated caller-owned configuration and fixed storage.
 * 中文：使用经验证的调用方配置与固定存储初始化一个 Node 实例。
 */
ucn_result_t ucn_node_init(ucn_node_t *node, const ucn_config_t *config)
{
    ucn_result_t result;

    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_validate_config(config);
    if (result != UCN_OK) {
        return result;
    }

    (void)memset(node, 0, sizeof(*node));
    node->config = *config;
    node->tx_wire_profile = UCN_WIRE_PROFILE_W3_BACKBONE;
    node->max_receive_wire_profile = UCN_WIRE_PROFILE_W3_BACKBONE;
    node->next_sequence = 1U;
    node->next_queue_order = 1U;
    node->next_route_request_id = 1U;
    node->next_route_epoch = 1U;
    node->next_heartbeat_id = 1U;
    node->control_tokens = UCN_CONTROL_TOKEN_BURST;
#if UCN_FEATURE_DYNAMIC_MESH
    node->default_route_constraints.max_hops = config->default_hop_limit;
    node->default_route_constraints.max_route_cost = UCN_ROUTE_COST_MAX;
#endif
#if UCN_FEATURE_DIAGNOSTICS
    node->next_path_trace_id = 1U;
    node->next_node_snapshot_id = 1U;
    node->next_policy_diagnostic_id = 1U;
    node->path_trace_tokens = UCN_PATH_TRACE_TOKEN_BURST;
    node->node_snapshot_tokens = UCN_NODE_SNAPSHOT_TOKEN_BURST;
    node->policy_diagnostic_tokens = UCN_POLICY_DIAGNOSTIC_TOKEN_BURST;
#endif
    node->security_policy.tx_mode = UCN_SECURITY_TX_PLAIN;
    node->security_policy.rx_mode = UCN_SECURITY_RX_BOTH;
    node->security_policy.forward_mode = UCN_SECURITY_FORWARD_PLAIN_AND_OPAQUE_E2E;
    node->security_required = UCN_SECURITY_REQUIRED_BY_DEFAULT != 0;
    return UCN_OK;
}

/*
 * EN: Validates and sets `wire_profiles` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `wire_profiles`。
 */
ucn_result_t ucn_node_set_wire_profiles(
    ucn_node_t *node,
    ucn_wire_profile_t tx_profile,
    ucn_wire_profile_t max_receive_profile)
{
    const ucn_wire_profile_descriptor_t *tx;
    const ucn_wire_profile_descriptor_t *rx;

    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (node->link_count != 0U || node->security_ops != NULL) {
        return UCN_ERR_CONFIG;
    }
    tx = ucn_wire_profile_get_descriptor(tx_profile);
    rx = ucn_wire_profile_get_descriptor(max_receive_profile);
    if (tx == NULL || rx == NULL || max_receive_profile < tx_profile) {
        return UCN_ERR_ARGUMENT;
    }
    if (node->config.network_id > tx->max_wire_value ||
        node->config.node_id > tx->max_node_id ||
        node->config.default_hop_limit > tx->max_hops ||
        node->session_id > tx->max_wire_value) {
        return UCN_ERR_TOO_LARGE;
    }
    node->tx_wire_profile = tx_profile;
    node->max_receive_wire_profile = max_receive_profile;
    return UCN_OK;
}

/*
 * EN: Returns the current `tx_wire_profile` view from Lite/Full Node state.
 * 中文：从 Lite/Full Node 状态返回当前 `tx_wire_profile` 视图。
 */
ucn_wire_profile_t ucn_node_get_tx_wire_profile(const ucn_node_t *node)
{
    return node == NULL ? UCN_WIRE_PROFILE_UNSPECIFIED : node->tx_wire_profile;
}

/*
 * EN: Returns the current `max_receive_wire_profile` view from Lite/Full Node state.
 * 中文：从 Lite/Full Node 状态返回当前 `max_receive_wire_profile` 视图。
 */
ucn_wire_profile_t ucn_node_get_max_receive_wire_profile(
    const ucn_node_t *node)
{
    return node == NULL ? UCN_WIRE_PROFILE_UNSPECIFIED :
                          node->max_receive_wire_profile;
}

/*
 * EN: Validates and sets `wire_profile_auto` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `wire_profile_auto`。
 */
ucn_result_t ucn_node_set_wire_profile_auto(ucn_node_t *node, bool enabled)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    node->automatic_wire_profile = enabled;
    return UCN_OK;
}

/*
 * EN: Returns whether route-aware automatic Wire-Profile selection is enabled.
 * 中文：返回是否已启用路由感知的自动 Wire Profile 选择。
 */
bool ucn_node_wire_profile_auto(const ucn_node_t *node)
{
    return node != NULL && node->automatic_wire_profile;
}

/*
 * EN: Validates and sets `link_wire_profile_limit` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `link_wire_profile_limit`。
 */
ucn_result_t ucn_node_set_link_wire_profile_limit(
    ucn_node_t *node,
    ucn_link_t *link,
    ucn_wire_profile_t maximum_profile)
{
    if (node == NULL || link == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!link_is_registered(node, link)) {
        return UCN_ERR_NOT_FOUND;
    }
    if (maximum_profile != UCN_WIRE_PROFILE_UNSPECIFIED &&
        ucn_wire_profile_get_descriptor(maximum_profile) == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    link->peer_wire_profile = maximum_profile;
    return UCN_OK;
}

/*
 * EN: Returns the current `link_wire_profile_limit` view from Lite/Full Node state.
 * 中文：从 Lite/Full Node 状态返回当前 `link_wire_profile_limit` 视图。
 */
ucn_wire_profile_t ucn_node_get_link_wire_profile_limit(
    const ucn_node_t *node,
    const ucn_link_t *link)
{
    return node == NULL || link == NULL || !link_is_registered(node, link) ?
               UCN_WIRE_PROFILE_UNSPECIFIED : link->peer_wire_profile;
}

/*
 * EN: Validates and sets `link_local_wire_profile_limit` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `link_local_wire_profile_limit`。
 */
ucn_result_t ucn_node_set_link_local_wire_profile_limit(
    ucn_node_t *node,
    ucn_link_t *link,
    ucn_wire_profile_t maximum_profile)
{
    ucn_wire_profile_t effective_profile = maximum_profile;

    if (node == NULL || link == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (maximum_profile == UCN_WIRE_PROFILE_UNSPECIFIED) {
        effective_profile = node->max_receive_wire_profile;
    } else if (ucn_wire_profile_get_descriptor(maximum_profile) == NULL ||
               maximum_profile > node->max_receive_wire_profile) {
        return UCN_ERR_ARGUMENT;
    }
    if (link->mtu != 0U &&
        link->mtu < ucn_frame_header_size_for_profile(effective_profile, 0U)) {
        return UCN_ERR_TOO_LARGE;
    }
    link->local_receive_wire_profile = (uint8_t)maximum_profile;
    return UCN_OK;
}

/*
 * EN: Returns the current `link_local_wire_profile_limit` view from Lite/Full Node state.
 * 中文：从 Lite/Full Node 状态返回当前 `link_local_wire_profile_limit` 视图。
 */
ucn_wire_profile_t ucn_node_get_link_local_wire_profile_limit(
    const ucn_node_t *node,
    const ucn_link_t *link)
{
    if (node == NULL || link == NULL) {
        return UCN_WIRE_PROFILE_UNSPECIFIED;
    }
    return (ucn_wire_profile_t)link->local_receive_wire_profile;
}

/*
 * EN: Validates and sets `plain_session_id` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `plain_session_id`。
 */
ucn_result_t ucn_node_set_plain_session_id(ucn_node_t *node,
                                           ucn_session_id_t session_id)
{
    if (node == NULL || session_id == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    if (node->security_ops != NULL) {
        return UCN_ERR_CONFIG;
    }
    if (session_id > ucn_wire_profile_get_descriptor(
                         node->tx_wire_profile)->max_wire_value) {
        return UCN_ERR_TOO_LARGE;
    }
    node->session_id = session_id;
    return UCN_OK;
}

/*
 * EN: Checks the current `security_ready` condition in Lite/Full Node state.
 * 中文：检查当前 Lite/Full Node 状态中的 `security_ready` 条件。
 */
bool ucn_node_security_ready(const ucn_node_t *node)
{
    size_t index;

    if (node == NULL) {
        return false;
    }
    if (!node->security_required) {
        return true;
    }
    if (node->security_ops == NULL || node->session_id == 0U ||
        node->security_ops->authorize_tx == NULL ||
        node->security_ops->authorize_rx == NULL ||
        node->security_ops->seal == NULL || node->security_ops->open == NULL ||
        !security_policy_is_production_ready(&node->security_policy)) {
        return false;
    }
    for (index = 0U; index < UCN_MAX_ENDPOINT_SECURITY_POLICIES; ++index) {
        if (node->endpoint_security_policies[index].occupied &&
            !security_policy_is_production_ready(
                &node->endpoint_security_policies[index].policy)) {
            return false;
        }
    }
    return true;
}

/*
 * EN: Validates and sets `security_required` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `security_required`。
 */
ucn_result_t ucn_node_set_security_required(ucn_node_t *node, bool required)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    node->security_required = required;
    return UCN_OK;
}

/*
 * EN: Validates and sets `security` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `security`。
 */
ucn_result_t ucn_node_set_security(ucn_node_t *node,
                                   const ucn_security_ops_t *ops,
                                   void *context)
{
    ucn_sequence_t next_sequence;
    ucn_session_id_t session_id;
    ucn_result_t result;

    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    if (ops == NULL) {
        node->security_ops = NULL;
        node->security_context = NULL;
        node->session_id = 0U;
        return UCN_OK;
    }

    if (ops->load_next_sequence == NULL || ops->store_next_sequence == NULL ||
        ops->get_session_id == NULL || ops->authorize_tx == NULL ||
        ops->authorize_rx == NULL || ((ops->seal == NULL) != (ops->open == NULL))) {
        return UCN_ERR_ARGUMENT;
    }
    if (node->security_required && (ops->seal == NULL || ops->open == NULL)) {
        return UCN_ERR_SECURITY;
    }

    result = ops->load_next_sequence(context, &next_sequence);
    if (result != UCN_OK || next_sequence == 0U || next_sequence == UINT32_MAX) {
        return result == UCN_OK ? UCN_ERR_SECURITY : result;
    }
    result = ops->get_session_id(context, &session_id);
    if (result != UCN_OK || session_id == 0U) {
        return result == UCN_OK ? UCN_ERR_SECURITY : result;
    }
    if (session_id > ucn_wire_profile_get_descriptor(
                         node->tx_wire_profile)->max_wire_value) {
        return UCN_ERR_TOO_LARGE;
    }

    node->security_ops = ops;
    node->security_context = context;
    node->next_sequence = next_sequence;
    node->session_id = session_id;
    return UCN_OK;
}

/*
 * EN: Validates and sets `security_policy` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `security_policy`。
 */
ucn_result_t ucn_node_set_security_policy(ucn_node_t *node,
                                          const ucn_security_policy_t *policy)
{
    if (node == NULL || !security_policy_is_valid(policy)) {
        return UCN_ERR_ARGUMENT;
    }
    node->security_policy = *policy;
    return UCN_OK;
}

/*
 * EN: Validates and sets `endpoint_security_policy` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `endpoint_security_policy`。
 */
ucn_result_t ucn_node_set_endpoint_security_policy(
    ucn_node_t *node,
    ucn_endpoint_t endpoint,
    const ucn_security_policy_t *policy)
{
    ucn_endpoint_security_policy_entry_t *entry;
    size_t index;

    if (node == NULL || !ucn_endpoint_is_static(endpoint)) {
        return UCN_ERR_ARGUMENT;
    }
    entry = find_endpoint_security_policy(node, endpoint);
    if (policy == NULL) {
        if (entry != NULL) {
            (void)memset(entry, 0, sizeof(*entry));
        }
        return UCN_OK;
    }
    if (!security_policy_is_valid(policy)) {
        return UCN_ERR_ARGUMENT;
    }
    if (entry != NULL) {
        entry->policy = *policy;
        return UCN_OK;
    }
    for (index = 0U; index < UCN_MAX_ENDPOINT_SECURITY_POLICIES; ++index) {
        if (!node->endpoint_security_policies[index].occupied) {
            node->endpoint_security_policies[index].occupied = true;
            node->endpoint_security_policies[index].endpoint = endpoint;
            node->endpoint_security_policies[index].policy = *policy;
            return UCN_OK;
        }
    }
    return UCN_ERR_NO_SPACE;
}

/*
 * EN: Validates and sets `join_policy` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `join_policy`。
 */
ucn_result_t ucn_node_set_join_policy(ucn_node_t *node,
                                      ucn_join_policy_t policy,
                                      ucn_neighbor_authorize_fn authorize,
                                      void *context)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (policy != UCN_JOIN_MANUAL && policy != UCN_JOIN_OPEN &&
        policy != UCN_JOIN_PROVIDER) {
        return UCN_ERR_ARGUMENT;
    }
    if (policy == UCN_JOIN_PROVIDER && authorize == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (policy != UCN_JOIN_PROVIDER && authorize != NULL) {
        return UCN_ERR_ARGUMENT;
    }

    node->join_policy = policy;
    node->neighbor_authorize = authorize;
    node->neighbor_authorize_context = context;
    return UCN_OK;
}

#if UCN_FEATURE_DIAGNOSTICS
/*
 * EN: Validates and sets `node_snapshot_authorizer` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `node_snapshot_authorizer`。
 */
ucn_result_t ucn_node_set_node_snapshot_authorizer(
    ucn_node_t *node,
    ucn_node_snapshot_authorize_fn authorize,
    void *context)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    node->node_snapshot_authorize = authorize;
    node->node_snapshot_authorize_context = context;
    return UCN_OK;
}

/*
 * EN: Validates and sets `path_trace_authorizer` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `path_trace_authorizer`。
 */
ucn_result_t ucn_node_set_path_trace_authorizer(
    ucn_node_t *node,
    ucn_path_trace_authorize_fn authorize,
    void *context)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    node->path_trace_authorize = authorize;
    node->path_trace_authorize_context = context;
    return UCN_OK;
}

/*
 * EN: Validates and sets `policy_diagnostic_authorizer` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `policy_diagnostic_authorizer`。
 */
ucn_result_t ucn_node_set_policy_diagnostic_authorizer(
    ucn_node_t *node,
    ucn_policy_diagnostic_authorize_fn authorize,
    void *context)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    node->policy_diagnostic_authorize = authorize;
    node->policy_diagnostic_authorize_context = context;
    return UCN_OK;
}
#endif

#if UCN_FEATURE_PATH
/*
 * EN: Validates and sets `path_control_authorizer` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `path_control_authorizer`。
 */
ucn_result_t ucn_node_set_path_control_authorizer(
    ucn_node_t *node,
    ucn_path_control_authorize_fn authorize,
    void *context)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    node->path_control_authorize = authorize;
    node->path_control_authorize_context = context;
    return UCN_OK;
}
#endif

/*
 * EN: Updates `observe_neighbor` in bounded Lite/Full Node state.
 * 中文：更新固定容量 Lite/Full Node 状态中的 `observe_neighbor`。
 */
ucn_result_t ucn_node_observe_neighbor(ucn_node_t *node,
                                       ucn_link_t *link,
                                       uint32_t now_ms)
{
    ucn_neighbor_entry_t *entry;
    ucn_neighbor_bearer_t *bearer;
    ucn_result_t result;

    if (node == NULL || link == NULL || link->peer_node_id == 0U ||
        link->peer_node_id == UCN_NODE_BROADCAST ||
        link->peer_node_id == node->config.node_id || link->ops == NULL ||
        link->ops->send == NULL || link->ops->get_status == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    expire_neighbor_candidates(node, now_ms);
    entry = find_neighbor(node, link->peer_node_id);
    if (entry != NULL) {
        bearer = find_neighbor_bearer(entry, link);
        if (bearer != NULL && bearer_is_active(bearer)) {
            bearer->last_seen_ms = now_ms;
            bearer->state = UCN_NEIGHBOR_BEARER_ADMITTED;
            entry->state = UCN_NEIGHBOR_ADMITTED;
            entry->suspect_since_ms = 0U;
            (void)select_neighbor_bearer(node, entry);
            return UCN_OK;
        }
        if (bearer != NULL && bearer->state == UCN_NEIGHBOR_BEARER_CANDIDATE) {
            if (entry->state == UCN_NEIGHBOR_REJECTED ||
                entry->state == UCN_NEIGHBOR_EXPIRED ||
                entry->state == UCN_NEIGHBOR_REMOVED) {
                entry->state = UCN_NEIGHBOR_CANDIDATE;
            }
            bearer->last_seen_ms = now_ms;
        } else if (bearer != NULL) {
            (void)memset(bearer, 0, sizeof(*bearer));
            bearer->state = UCN_NEIGHBOR_BEARER_CANDIDATE;
            bearer->link = link;
            bearer->last_seen_ms = now_ms;
        } else if (entry->state == UCN_NEIGHBOR_ADMITTED ||
                   entry->state == UCN_NEIGHBOR_SUSPECT ||
                   entry->state == UCN_NEIGHBOR_CANDIDATE) {
            bearer = allocate_neighbor_bearer(entry);
            if (bearer == NULL) {
                return UCN_ERR_NO_SPACE;
            }
            (void)memset(bearer, 0, sizeof(*bearer));
            bearer->state = UCN_NEIGHBOR_BEARER_CANDIDATE;
            bearer->link = link;
            bearer->last_seen_ms = now_ms;
            ++entry->bearer_count;
        } else {
            (void)memset(entry, 0, sizeof(*entry));
            entry->state = UCN_NEIGHBOR_CANDIDATE;
            entry->peer_node_id = link->peer_node_id;
            entry->primary_bearer_index = UCN_NEIGHBOR_PRIMARY_BEARER_NONE;
            bearer = &entry->bearers[0];
            bearer->state = UCN_NEIGHBOR_BEARER_CANDIDATE;
            bearer->link = link;
            bearer->last_seen_ms = now_ms;
            entry->bearer_count = 1U;
        }
    } else {
        entry = allocate_neighbor_slot(node);
        if (entry == NULL) {
            return UCN_ERR_NO_SPACE;
        }
        (void)memset(entry, 0, sizeof(*entry));
        entry->state = UCN_NEIGHBOR_CANDIDATE;
        entry->peer_node_id = link->peer_node_id;
        entry->primary_bearer_index = UCN_NEIGHBOR_PRIMARY_BEARER_NONE;
        bearer = &entry->bearers[0];
        bearer->state = UCN_NEIGHBOR_BEARER_CANDIDATE;
        bearer->link = link;
        bearer->last_seen_ms = now_ms;
        entry->bearer_count = 1U;
    }

    if (node->join_policy == UCN_JOIN_MANUAL) {
        return UCN_OK;
    }
    if (node->join_policy == UCN_JOIN_PROVIDER) {
        result = node->neighbor_authorize(node->neighbor_authorize_context,
                                          node->config.node_id,
                                          entry->peer_node_id,
                                          bearer->link);
        if (result != UCN_OK) {
            if (entry->state == UCN_NEIGHBOR_CANDIDATE) {
                entry->state = UCN_NEIGHBOR_REJECTED;
            } else {
                (void)memset(bearer, 0, sizeof(*bearer));
                --entry->bearer_count;
            }
            return result;
        }
    }
    result = admit_neighbor_entry(node, entry);
    if (result == UCN_OK) {
        (void)select_neighbor_bearer(node, entry);
    }
    return result;
}

/*
 * EN: Builds and submits `probe_neighbor` through the bounded Lite/Full Node transmit path.
 * 中文：构造 `probe_neighbor` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
ucn_result_t ucn_node_probe_neighbor(ucn_node_t *node,
                                     ucn_link_t *link,
                                     uint32_t now_ms)
{
    uint8_t payload;
    ucn_wire_profile_t local_receive_profile;
    ucn_result_t result;

    result = ucn_node_observe_neighbor(node, link, now_ms);
    if (result != UCN_OK) {
        return result;
    }

    node->now_ms = now_ms;
    result = resolve_link_local_receive_profile(node, link,
                                                &local_receive_profile);
    if (result != UCN_OK) {
        return result;
    }
    payload = (uint8_t)local_receive_profile;
    return send_adaptive_control_on_link(
        node, link, link->peer_node_id, UCN_MSG_HELLO, &payload,
        (uint16_t)UCN_HELLO_PAYLOAD_BYTES);
}

/*
 * EN: Builds and submits `broadcast_hello` through the bounded Lite/Full Node transmit path.
 * 中文：构造 `broadcast_hello` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
ucn_result_t ucn_node_broadcast_hello(ucn_node_t *node,
                                      ucn_link_t *link,
                                      uint32_t now_ms)
{
    uint8_t payload;
    ucn_wire_profile_t local_receive_profile;
    ucn_result_t result;

    if (node == NULL || link == NULL || link->ops == NULL ||
        link->ops->send == NULL || link->ops->get_status == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    node->now_ms = now_ms;
    result = resolve_link_local_receive_profile(node, link,
                                                &local_receive_profile);
    if (result != UCN_OK) {
        return result;
    }
    payload = (uint8_t)local_receive_profile;
    return send_adaptive_control_on_link(
        node, link, UCN_NODE_BROADCAST, UCN_MSG_HELLO, &payload,
        (uint16_t)UCN_HELLO_PAYLOAD_BYTES);
}

/*
 * EN: Validates and installs `admit_neighbor` into bounded Lite/Full Node state.
 * 中文：验证 `admit_neighbor` 并将其安装到固定容量的 Lite/Full Node 状态中。
 */
ucn_result_t ucn_node_admit_neighbor(ucn_node_t *node,
                                     ucn_node_id_t peer_node_id)
{
    ucn_neighbor_entry_t *entry;

    if (node == NULL || peer_node_id == 0U || peer_node_id == UCN_NODE_BROADCAST) {
        return UCN_ERR_ARGUMENT;
    }
    entry = find_neighbor(node, peer_node_id);
    if (entry == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    return admit_neighbor_entry(node, entry);
}

/*
 * EN: Removes or releases `reject_neighbor` from Lite/Full Node state with bounded work.
 * 中文：以有界工作量从 Lite/Full Node 状态移除或释放 `reject_neighbor`。
 */
ucn_result_t ucn_node_reject_neighbor(ucn_node_t *node,
                                      ucn_node_id_t peer_node_id)
{
    ucn_neighbor_entry_t *entry;

    if (node == NULL || peer_node_id == 0U || peer_node_id == UCN_NODE_BROADCAST) {
        return UCN_ERR_ARGUMENT;
    }
    entry = find_neighbor(node, peer_node_id);
    if (entry == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    if (entry->state == UCN_NEIGHBOR_ADMITTED ||
        entry->state == UCN_NEIGHBOR_SUSPECT) {
        return UCN_ERR_UNSUPPORTED;
    }
    entry->state = UCN_NEIGHBOR_REJECTED;
    return UCN_OK;
}

/*
 * EN: Calculates the bounded `neighbor_count` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `neighbor_count` 值。
 */
size_t ucn_node_neighbor_count(const ucn_node_t *node,
                               ucn_neighbor_state_t state)
{
    size_t index;
    size_t count = 0U;

    if (node == NULL || state == UCN_NEIGHBOR_EMPTY) {
        return 0U;
    }
    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        if (node->neighbors[index].state == state) {
            ++count;
        }
    }
    return count;
}

/*
 * EN: Copies `neighbor_summaries` from Lite/Full Node into caller-owned storage.
 * 中文：把 Lite/Full Node 中的 `neighbor_summaries` 复制到调用方存储。
 */
size_t ucn_node_copy_neighbor_summaries(
    const ucn_node_t *node,
    ucn_neighbor_summary_t *output,
    size_t capacity)
{
    size_t index;
    size_t count = 0U;

    if (node == NULL || (output == NULL && capacity != 0U)) {
        return 0U;
    }
    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        const ucn_neighbor_entry_t *entry = &node->neighbors[index];
        size_t bearer_index;
        uint32_t freshest = 0U;

        if (entry->state == UCN_NEIGHBOR_EMPTY) {
            continue;
        }
        if (output == NULL) {
            ++count;
            continue;
        }
        if (count >= capacity) {
            break;
        }
        for (bearer_index = 0U; bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR;
             ++bearer_index) {
            const ucn_neighbor_bearer_t *bearer = &entry->bearers[bearer_index];

            if (bearer->state != UCN_NEIGHBOR_BEARER_EMPTY &&
                (freshest == 0U ||
                 (int32_t)(bearer->last_seen_ms - freshest) > 0)) {
                freshest = bearer->last_seen_ms;
            }
        }
        output[count].state = entry->state;
        output[count].peer_node_id = entry->peer_node_id;
        output[count].bearer_count = entry->bearer_count;
        output[count].primary_bearer_index = entry->primary_bearer_index;
        output[count].last_seen_ms = freshest;
        ++count;
    }
    return count;
}

/*
 * EN: Validates and installs `register_link` into bounded Lite/Full Node state.
 * 中文：验证 `register_link` 并将其安装到固定容量的 Lite/Full Node 状态中。
 */
ucn_result_t ucn_node_register_link(ucn_node_t *node, ucn_link_t *link)
{
    size_t index;
    ucn_wire_profile_t local_receive_profile;
    ucn_result_t result;

    if (node == NULL || link == NULL || link->ops == NULL ||
        link->ops->send == NULL || link->ops->get_status == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_link_liveness_profile_is_valid(link->liveness_profile)) {
        return UCN_ERR_CONFIG;
    }
    result = resolve_link_local_receive_profile(node, link,
                                                &local_receive_profile);
    if (result != UCN_OK) {
        return result;
    }
    if (link->mtu != 0U &&
        link->mtu < ucn_frame_header_size_for_profile(local_receive_profile,
                                                       0U)) {
        return UCN_ERR_ARGUMENT;
    }

    for (index = 0U; index < node->link_count; ++index) {
        if (node->links[index] == link ||
            node->links[index]->link_id == link->link_id) {
            return UCN_ERR_CONFIG;
        }
    }

    if (node->link_count >= UCN_MAX_LINKS) {
        return UCN_ERR_NO_SPACE;
    }

    if (link->ops->open != NULL) {
        result = link->ops->open(link);
        if (result != UCN_OK) {
            return result;
        }
    }

    node->links[node->link_count] = link;
    link->peer_wire_profile = UCN_WIRE_PROFILE_UNSPECIFIED;
    ++node->link_count;
    return UCN_OK;
}

/*
 * EN: Validates and installs `add_route` into bounded Lite/Full Node state.
 * 中文：验证 `add_route` 并将其安装到固定容量的 Lite/Full Node 状态中。
 */
ucn_result_t ucn_node_add_route(ucn_node_t *node,
                                ucn_node_id_t destination,
                                ucn_link_t *egress_link)
{
    size_t index;

    if (node == NULL || destination == 0U || destination == UCN_NODE_BROADCAST ||
        egress_link == NULL || !link_is_registered(node, egress_link)) {
        return UCN_ERR_ARGUMENT;
    }

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid &&
            node->routes[index].destination == destination) {
            node->routes[index].is_static = true;
            node->routes[index].egress_link = egress_link;
            node->routes[index].expires_at_ms = 0U;
            node->routes[index].route_cost = link_route_cost(egress_link);
            node->routes[index].hop_count = 1U;
            node->routes[index].verified_rtt_valid = false;
            node->routes[index].verified_rtt_ms = 0U;
            return UCN_OK;
        }
    }

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (!node->routes[index].valid) {
            node->routes[index].valid = true;
            node->routes[index].is_static = true;
            node->routes[index].destination = destination;
            node->routes[index].egress_link = egress_link;
            node->routes[index].expires_at_ms = 0U;
            node->routes[index].route_cost = link_route_cost(egress_link);
            node->routes[index].hop_count = 1U;
            node->routes[index].verified_rtt_valid = false;
            node->routes[index].verified_rtt_ms = 0U;
            return UCN_OK;
        }
    }

    return UCN_ERR_NO_SPACE;
}

/*
 * EN: Validates and sets `default_route_constraints` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `default_route_constraints`。
 */
ucn_result_t ucn_node_set_default_route_constraints(
    ucn_node_t *node,
    const ucn_route_constraints_t *constraints)
{
#if UCN_FEATURE_DYNAMIC_MESH
    ucn_route_constraints_t resolved;

    if (node == NULL || constraints == NULL ||
        constraints->max_hops > node->config.default_hop_limit ||
        constraints->max_route_cost == UCN_ROUTE_COST_UNKNOWN) {
        return UCN_ERR_ARGUMENT;
    }
    resolved = *constraints;
    if (resolved.max_hops == 0U) {
        resolved.max_hops = node->config.default_hop_limit;
    }
    if (resolved.max_route_cost == 0U) {
        resolved.max_route_cost = UCN_ROUTE_COST_MAX;
    }
    node->default_route_constraints = resolved;
    return UCN_OK;
#else
    (void)constraints;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
#endif
}

/*
 * EN: Returns the current `default_route_constraints` view from Lite/Full Node state.
 * 中文：从 Lite/Full Node 状态返回当前 `default_route_constraints` 视图。
 */
ucn_result_t ucn_node_get_default_route_constraints(
    const ucn_node_t *node,
    ucn_route_constraints_t *constraints)
{
#if UCN_FEATURE_DYNAMIC_MESH
    if (node == NULL || constraints == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    *constraints = node->default_route_constraints;
    return UCN_OK;
#else
    (void)constraints;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
#endif
}

/*
 * EN: Returns the current `route_quality` view from Lite/Full Node state.
 * 中文：从 Lite/Full Node 状态返回当前 `route_quality` 视图。
 */
ucn_result_t ucn_node_get_route_quality(const ucn_node_t *node,
                                        ucn_node_id_t destination,
                                        ucn_route_quality_t *quality)
{
#if UCN_FEATURE_DYNAMIC_MESH
    const ucn_route_entry_t *route = NULL;
    const ucn_link_t *best_direct = NULL;
    ucn_route_cost_t best_direct_cost = UCN_ROUTE_COST_UNKNOWN;
    size_t index;

    if (node == NULL || quality == NULL || destination == 0U ||
        destination == UCN_NODE_BROADCAST) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(quality, 0, sizeof(*quality));
    for (index = 0U; index < node->link_count; ++index) {
        const ucn_link_t *link = node->links[index];

        if (link->peer_node_id == destination && link_is_usable(link)) {
            const ucn_route_cost_t cost = link_route_cost(link);

            if (best_direct == NULL ||
                route_cost_is_better(cost, best_direct_cost)) {
                best_direct = link;
                best_direct_cost = cost;
            }
        }
    }
    if (best_direct != NULL) {
        ucn_link_metrics_t metrics;

        quality->available = true;
        quality->hop_count = 1U;
        quality->route_cost = best_direct_cost;
        (void)memset(&metrics, 0, sizeof(metrics));
        if (best_direct->ops->get_metrics != NULL &&
            best_direct->ops->get_metrics(best_direct, &metrics) == UCN_OK &&
            metrics.rtt_valid) {
            quality->verified_rtt_valid = true;
            quality->verified_rtt_ms = metrics.rtt_ms;
        }
        return UCN_OK;
    }
    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid &&
            node->routes[index].destination == destination &&
            !route_is_expired(node, &node->routes[index])) {
            route = &node->routes[index];
            break;
        }
    }
    if (route == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    quality->available = true;
    quality->hop_count = route->hop_count;
    quality->route_cost = route->route_cost;
    quality->verified_rtt_valid = route->verified_rtt_valid;
    quality->verified_rtt_ms = route->verified_rtt_ms;
    return UCN_OK;
#else
    (void)destination;
    (void)quality;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
#endif
}

/*
 * EN: Initializes `initial_route_discovery_hop_limit` for Lite/Full Node using caller-owned fixed storage.
 * 中文：使用调用方提供的固定存储初始化 Lite/Full Node 的 `initial_route_discovery_hop_limit`。
 */
static uint8_t initial_route_discovery_hop_limit(uint8_t maximum_hop_limit)
{
    return maximum_hop_limit < 2U ? maximum_hop_limit : 2U;
}

/*
 * EN: Derives `next_route_discovery_hop_limit` without unbounded work or allocation in Lite/Full Node.
 * 中文：在 Lite/Full Node 中以无动态分配的有界方式推导 `next_route_discovery_hop_limit`。
 */
static uint8_t next_route_discovery_hop_limit(uint8_t current_hop_limit,
                                              uint8_t maximum_hop_limit)
{
    const uint16_t doubled = (uint16_t)current_hop_limit * 2U;

    return doubled >= maximum_hop_limit ? maximum_hop_limit : (uint8_t)doubled;
}

/*
 * EN: Validates and submits `send_route_discovery_ring` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_route_discovery_ring` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_route_discovery_ring(ucn_node_t *node,
                                              ucn_route_discovery_t *slot,
                                              uint8_t hop_limit,
                                              uint32_t now_ms,
                                              bool is_expansion)
{
    uint8_t payload[UCN_ROUTE_REQ_MAX_PAYLOAD_BYTES];
    ucn_frame_t frame;
    ucn_wire_profile_t request_profile;
    ucn_rreq_cache_classification_t cache_classification;
    size_t cache_slot = 0U;
    ucn_result_t result;

    if (node == NULL || slot == NULL || hop_limit == 0U ||
        hop_limit > slot->maximum_hop_limit) {
        return UCN_ERR_ARGUMENT;
    }
    result = select_route_request_profile(node, slot->destination, hop_limit,
                                          &request_profile);
    if (result != UCN_OK) {
        return result;
    }
    if (!take_control_token(node)) {
        return UCN_ERR_NO_SPACE;
    }

    if (node->next_route_request_id == 0U) {
        node->next_route_request_id = 1U;
    }
    slot->active = true;
    slot->request_id = node->next_route_request_id++;
    slot->started_at_ms = now_ms;
    slot->deadline_ms =
        ucn_deadline_from_now(now_ms, UCN_ROUTE_RING_TIMEOUT_MS);
    slot->current_hop_limit = hop_limit;

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_MSG_ROUTE_REQ;
    frame.wire_profile = request_profile;
    frame.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    frame.hop_limit = hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = UCN_NODE_BROADCAST;
    frame.session_id = node->session_id;
    write_uint_be(payload,
                  ucn_wire_profile_get_descriptor(frame.wire_profile)->address_bytes,
                  slot->destination);
    write_u32_be(payload + route_request_id_offset(&frame), slot->request_id);
    result = write_route_cost_for_profile(
        payload + route_request_cost_offset(&frame), frame.wire_profile, 0U);
    if (result != UCN_OK) {
        slot->active = false;
        return result;
    }
    payload[route_request_hop_offset(&frame)] = 0U;
    payload[route_request_flags_offset(&frame)] =
#if UCN_FEATURE_CANDIDATE_ROUTING
        slot->is_candidate ? UCN_ROUTE_REQ_FLAG_CANDIDATE : 0U;
#else
        0U;
#endif
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        slot->active = false;
        return result;
    }
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length =
        (uint16_t)route_request_payload_size(frame.wire_profile);

    /* Remember the origin's own flood before the first Link send. A
     * multi-bearer neighbour can legitimately receive the RREQ on one bearer
     * and re-flood it over another; without this origin entry the returned
     * copy is learned as a route to self. */
    cache_classification = classify_route_request(node, &frame, 0U,
                                                  &cache_slot);
    if (cache_classification == UCN_RREQ_CACHE_FULL) {
        slot->active = false;
        node->stats.route_request_cache_full++;
        return UCN_ERR_NO_SPACE;
    }
    if (cache_classification == UCN_RREQ_CACHE_REPLAY) {
        slot->active = false;
        return UCN_ERR_REPLAY;
    }
    commit_route_request(node, &frame, 0U, cache_slot);

    result = forward_route_request(node, NULL, &frame);
    if (result != UCN_OK) {
        slot->active = false;
        return result;
    }
    node->stats.route_requests_sent++;
    if (is_expansion) {
        node->stats.route_request_ring_expansions++;
    }
    return UCN_OK;
}

/*
 * EN: Validates and submits `send_due_route_discovery_ring` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_due_route_discovery_ring` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_due_route_discovery_ring(ucn_node_t *node,
                                                   uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        ucn_route_discovery_t *slot = &node->discoveries[index];
        uint8_t next_hop_limit;

        if (!slot->active ||
            !ucn_deadline_expired(now_ms, slot->deadline_ms) ||
            slot->current_hop_limit >= slot->maximum_hop_limit) {
            continue;
        }
        next_hop_limit = next_route_discovery_hop_limit(
            slot->current_hop_limit, slot->maximum_hop_limit);
        return send_route_discovery_ring(node, slot, next_hop_limit, now_ms,
                                         true);
    }
    return UCN_ERR_NOT_FOUND;
}

/*
 * EN: Starts or prepares `begin_route_discovery` after validating Lite/Full Node prerequisites.
 * 中文：验证 Lite/Full Node 前置条件后启动或准备 `begin_route_discovery`。
 */
static ucn_result_t begin_route_discovery(ucn_node_t *node,
                                          ucn_node_id_t destination,
                                          uint32_t now_ms,
                                          bool is_candidate,
                                          uint8_t maximum_hop_limit,
                                          bool restart_active,
                                          bool require_verified_rtt)
{
    ucn_route_discovery_t *slot = NULL;
    uint8_t initial_hop_limit;
    size_t index;
    ucn_result_t result;

    if (node == NULL || destination == 0U || destination == UCN_NODE_BROADCAST ||
        destination == node->config.node_id) {
        return UCN_ERR_ARGUMENT;
    }
    expire_dynamic_state(node, now_ms);
#if !UCN_FEATURE_CANDIDATE_ROUTING
    if (is_candidate) {
        return UCN_ERR_CONFIG;
    }
#else
    if (!is_candidate && find_link(node, destination) != NULL) {
        return UCN_OK;
    }
    if (is_candidate) {
        ucn_route_entry_t *active_route = find_active_route(node, destination);

        if (active_route == NULL || active_route->is_static) {
            return UCN_ERR_NOT_FOUND;
        }
        initial_hop_limit = active_route->hop_count;
    } else
#endif
    {
        if (find_link(node, destination) != NULL) {
            return UCN_OK;
        }
        initial_hop_limit = initial_route_discovery_hop_limit(
            node->config.default_hop_limit);
    }

    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        if (node->discoveries[index].active &&
            node->discoveries[index].destination == destination
#if UCN_FEATURE_CANDIDATE_ROUTING
            && node->discoveries[index].is_candidate == is_candidate
#endif
            ) {
#if UCN_FEATURE_CANDIDATE_ROUTING
            node->discoveries[index].require_verified_rtt =
                node->discoveries[index].require_verified_rtt ||
                require_verified_rtt;
#else
            (void)require_verified_rtt;
#endif
            if (!restart_active) {
                return UCN_OK;
            }
            if ((uint32_t)(now_ms - node->discoveries[index].started_at_ms) <
                UCN_ROUTE_REQUEST_MIN_INTERVAL_MS) {
                return UCN_OK;
            }
            node->discoveries[index].overall_started_at_ms = now_ms;
            return send_route_discovery_ring(
                node, &node->discoveries[index],
                node->discoveries[index].current_hop_limit, now_ms, false);
        }
        if (!node->discoveries[index].active && slot == NULL) {
            slot = &node->discoveries[index];
        }
    }
    if (slot == NULL) {
        return UCN_ERR_NO_SPACE;
    }

    (void)memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->destination = destination;
    slot->overall_started_at_ms = now_ms;
    slot->maximum_hop_limit = maximum_hop_limit == 0U ?
                                  node->config.default_hop_limit :
                                  maximum_hop_limit;
#if UCN_FEATURE_CANDIDATE_ROUTING
    slot->is_candidate = is_candidate;
    slot->require_verified_rtt = require_verified_rtt;
#else
    (void)require_verified_rtt;
#endif
    if (initial_hop_limit == 0U || initial_hop_limit > slot->maximum_hop_limit) {
        initial_hop_limit = slot->maximum_hop_limit;
    }
    result = send_route_discovery_ring(node, slot, initial_hop_limit, now_ms,
                                       false);
    if (result != UCN_OK) {
        slot->active = false;
        return result;
    }
#if UCN_FEATURE_CANDIDATE_ROUTING
    if (is_candidate) {
        ucn_route_entry_t *active_route = find_active_route(node, destination);

        node->stats.route_refreshes_started++;
        if (active_route != NULL) {
            active_route->last_refresh_started_ms = now_ms;
        }
    }
#endif
    return UCN_OK;
}

#if UCN_FEATURE_DYNAMIC_MESH
/* EN: A Q0 item may encounter the short gap between an expiring active Route
 * and its replacement. Candidate refresh and ordinary discovery share the
 * same destination contract here, so avoid a second slot or duplicate ring.
 * 中文：Q0 项可能遇到活动路由过期与替代路由安装之间的短暂空窗；候选刷新和
 * 普通发现共享同一目的地事务，因此不得重复分配槽位或发送首轮发现。 */
static ucn_result_t ensure_q0_route_discovery(ucn_node_t *node,
                                              ucn_node_id_t destination,
                                              uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        if (node->discoveries[index].active &&
            node->discoveries[index].destination == destination) {
            return UCN_OK;
        }
    }
    return begin_route_discovery(node, destination, now_ms, false, 0U, false,
                                 false);
}
#endif

/*
 * EN: Builds and submits `discover_route` through the bounded Lite/Full Node transmit path.
 * 中文：构造 `discover_route` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
ucn_result_t ucn_node_discover_route(ucn_node_t *node,
                                     ucn_node_id_t destination,
                                     uint32_t now_ms)
{
    return begin_route_discovery(node, destination, now_ms, false, 0U, true,
                                 false);
}

/*
 * EN: Updates `refresh_route` in bounded Lite/Full Node state.
 * 中文：更新固定容量 Lite/Full Node 状态中的 `refresh_route`。
 */
ucn_result_t ucn_node_refresh_route(ucn_node_t *node,
                                    ucn_node_id_t destination,
                                    uint32_t now_ms)
{
#if UCN_FEATURE_CANDIDATE_ROUTING
    return begin_route_discovery(node, destination, now_ms, true, 0U, true,
                                 false);
#else
    (void)destination;
    (void)now_ms;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
#endif
}

/*
 * EN: Checks the current `route_pending` condition in Lite/Full Node state.
 * 中文：检查当前 Lite/Full Node 状态中的 `route_pending` 条件。
 */
bool ucn_node_route_pending(const ucn_node_t *node, ucn_node_id_t destination)
{
    size_t index;

    if (node == NULL) {
        return false;
    }
    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        if (node->discoveries[index].active &&
            node->discoveries[index].destination == destination) {
            return true;
        }
    }
    return false;
}

/*
 * EN: Validates and sets `rx_handler` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `rx_handler`。
 */
void ucn_node_set_rx_handler(ucn_node_t *node,
                             ucn_rx_handler_t handler,
                             void *context)
{
    if (node != NULL) {
        node->rx_handler = handler;
        node->rx_context = context;
    }
}

/*
 * EN: Validates and sets `endpoint_handler` in Lite/Full Node state.
 * 中文：验证并设置 Lite/Full Node 状态中的 `endpoint_handler`。
 */
ucn_result_t ucn_node_set_endpoint_handler(ucn_node_t *node,
                                            ucn_endpoint_t endpoint,
                                            ucn_endpoint_rx_handler_t handler,
                                            void *context)
{
    ucn_endpoint_handler_entry_t *entry;
    size_t index;

    if (node == NULL || !ucn_endpoint_is_static(endpoint)) {
        return UCN_ERR_ARGUMENT;
    }
    entry = find_endpoint_handler(node, endpoint);
    if (entry != NULL) {
        if (handler == NULL) {
            (void)memset(entry, 0, sizeof(*entry));
        } else {
            entry->handler = handler;
            entry->context = context;
        }
        return UCN_OK;
    }
    if (handler == NULL) {
        return UCN_OK;
    }
    for (index = 0U; index < UCN_MAX_ENDPOINT_HANDLERS; ++index) {
        if (!node->endpoint_handlers[index].occupied) {
            node->endpoint_handlers[index].occupied = true;
            node->endpoint_handlers[index].endpoint = endpoint;
            node->endpoint_handlers[index].handler = handler;
            node->endpoint_handlers[index].context = context;
            return UCN_OK;
        }
    }
    return UCN_ERR_NO_SPACE;
}

#if UCN_FEATURE_PATH
/*
 * EN: Validates and installs `install_local_path` into bounded Lite/Full Node state.
 * 中文：验证 `install_local_path` 并将其安装到固定容量的 Lite/Full Node 状态中。
 */
ucn_result_t ucn_node_install_local_path(ucn_node_t *node,
                                         ucn_path_id_t path_id,
                                         ucn_node_id_t destination,
                                         ucn_node_id_t next_hop,
                                         uint8_t remaining_hops,
                                         uint32_t lease_ms)
{
    return ucn_node_install_local_path_capable(
        node, path_id, destination, next_hop, remaining_hops, lease_ms, NULL);
}

/*
 * EN: Validates and installs `install_local_path_capable` into bounded Lite/Full Node state.
 * 中文：验证 `install_local_path_capable` 并将其安装到固定容量的 Lite/Full Node 状态中。
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
    ucn_result_t result;

    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (remaining_hops > node->config.default_hop_limit) {
        return UCN_ERR_TTL;
    }
    result = validate_path_wire_scope(node->tx_wire_profile, path_id,
                                      destination, next_hop);
    if (result != UCN_OK) {
        return result;
    }
    if (node->security_ops == NULL || node->session_id == 0U) {
        return UCN_ERR_SECURITY;
    }
    return install_path_forward_entry(node, node->config.node_id, node->session_id,
                                      path_id, destination, next_hop,
                                      remaining_hops, lease_ms, capability);
}

/*
 * EN: Removes or releases `revoke_local_path` from Lite/Full Node state with bounded work.
 * 中文：以有界工作量从 Lite/Full Node 状态移除或释放 `revoke_local_path`。
 */
ucn_result_t ucn_node_revoke_local_path(ucn_node_t *node,
                                        ucn_path_id_t path_id,
                                        ucn_node_id_t destination)
{
    ucn_result_t result;

    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = validate_path_wire_scope(node->tx_wire_profile, path_id,
                                      destination, 0U);
    if (result != UCN_OK) {
        return result;
    }
    if (node->security_ops == NULL || node->session_id == 0U) {
        return UCN_ERR_SECURITY;
    }
    return ucn_path_revoke(&node->path_state, node->config.node_id,
                           node->session_id, path_id, destination);
}

/*
 * EN: Validates and submits `send_path_install_internal` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_path_install_internal` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_path_install_internal(
    ucn_node_t *node,
    ucn_node_id_t control_target,
    ucn_path_id_t path_id,
    ucn_node_id_t destination,
    ucn_node_id_t next_hop,
    uint8_t remaining_hops,
    uint32_t lease_ms,
    const ucn_path_capability_t *capability,
    bool capable_schema)
{
    ucn_wire_profile_t wire_profile;
    const ucn_wire_profile_descriptor_t *descriptor;
    ucn_link_t *link;
    uint8_t payload[UCN_PATH_INSTALL_MAX_PAYLOAD_BYTES];
    size_t payload_length;
    ucn_result_t result;

    if (node == NULL || control_target == 0U ||
        control_target == UCN_NODE_BROADCAST || control_target == node->config.node_id ||
        path_id == 0U || destination == 0U || destination == UCN_NODE_BROADCAST ||
        !ucn_duration_is_valid(lease_ms) ||
        (next_hop == 0U && remaining_hops != 0U) ||
        (next_hop != 0U && remaining_hops == 0U) ||
        (next_hop == 0U && control_target != destination)) {
        return UCN_ERR_ARGUMENT;
    }
    if (remaining_hops > UCN_MAX_HOPS ||
        remaining_hops > node->config.default_hop_limit) {
        return UCN_ERR_TTL;
    }
    if (capability != NULL &&
        (capability->minimum_mtu == 0U ||
         ucn_wire_profile_get_descriptor(
             capability->maximum_wire_profile) == NULL)) {
        return UCN_ERR_ARGUMENT;
    }
    if (node->security_ops == NULL || node->session_id == 0U) {
        return UCN_ERR_SECURITY;
    }
    result = validate_path_wire_scope(node->tx_wire_profile, path_id,
                                      destination, next_hop);
    if (result != UCN_OK) {
        return result;
    }
    link = find_link(node, control_target);
    if (link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    result = select_path_control_profile(
        node, link, control_target, UCN_MSG_PATH_INSTALL, path_id,
        destination, next_hop, capable_schema, &wire_profile);
    if (result != UCN_OK) {
        return result;
    }
    descriptor = ucn_wire_profile_get_descriptor(wire_profile);
    payload_length = capable_schema ?
        path_install_capable_payload_size(wire_profile) :
        path_install_base_payload_size(wire_profile);
    if (descriptor == NULL || payload_length > sizeof(payload) ||
        remaining_hops > descriptor->max_hops) {
        return UCN_ERR_CONFIG;
    }
    write_uint_be(payload, descriptor->path_id_bytes, path_id);
    write_uint_be(payload + path_install_destination_offset(descriptor),
                  descriptor->address_bytes, destination);
    write_uint_be(payload + path_install_next_hop_offset(descriptor),
                  descriptor->address_bytes, next_hop);
    write_u32_be(payload + path_install_lease_offset(descriptor), lease_ms);
    payload[path_install_remaining_hops_offset(descriptor)] = remaining_hops;
    if (capable_schema) {
        payload[path_install_maximum_profile_offset(descriptor)] =
            (uint8_t)(capability == NULL ? UCN_WIRE_PROFILE_UNSPECIFIED :
                      capability->maximum_wire_profile);
        write_u16_be(payload + path_install_minimum_mtu_offset(descriptor),
                     capability == NULL ? 0U : capability->minimum_mtu);
    }
    result = send_control_to_node_profile(
        node, control_target, UCN_MSG_PATH_INSTALL, payload,
        (uint16_t)payload_length, wire_profile);
    if (result == UCN_OK) {
        node->stats.path_installs_sent++;
    }
    return result;
}

/*
 * EN: Validates and submits `send_path_install` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_path_install` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
ucn_result_t ucn_node_send_path_install(ucn_node_t *node,
                                        ucn_node_id_t control_target,
                                        ucn_path_id_t path_id,
                                        ucn_node_id_t destination,
                                        ucn_node_id_t next_hop,
                                        uint8_t remaining_hops,
                                        uint32_t lease_ms)
{
    return send_path_install_internal(
        node, control_target, path_id, destination, next_hop, remaining_hops,
        lease_ms, NULL, false);
}

/*
 * EN: Validates and submits `send_path_install_capable` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_path_install_capable` 并将其提交到有界的 Lite/Full Node 发送路径。
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
    return send_path_install_internal(
        node, control_target, path_id, destination, next_hop, remaining_hops,
        lease_ms, capability, true);
}

/*
 * EN: Validates and submits `send_path_revoke` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_path_revoke` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
ucn_result_t ucn_node_send_path_revoke(ucn_node_t *node,
                                       ucn_node_id_t control_target,
                                       ucn_path_id_t path_id,
                                       ucn_node_id_t destination)
{
    ucn_wire_profile_t wire_profile;
    const ucn_wire_profile_descriptor_t *descriptor;
    ucn_link_t *link;
    uint8_t payload[UCN_PATH_REVOKE_MAX_PAYLOAD_BYTES];
    size_t payload_length;
    ucn_result_t result;

    if (node == NULL || control_target == 0U ||
        control_target == UCN_NODE_BROADCAST || control_target == node->config.node_id ||
        path_id == 0U || destination == 0U || destination == UCN_NODE_BROADCAST) {
        return UCN_ERR_ARGUMENT;
    }
    if (node->security_ops == NULL || node->session_id == 0U) {
        return UCN_ERR_SECURITY;
    }
    result = validate_path_wire_scope(node->tx_wire_profile, path_id,
                                      destination, 0U);
    if (result != UCN_OK) {
        return result;
    }
    link = find_link(node, control_target);
    if (link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    result = select_path_control_profile(
        node, link, control_target, UCN_MSG_PATH_REVOKE, path_id,
        destination, 0U, false, &wire_profile);
    if (result != UCN_OK) {
        return result;
    }
    descriptor = ucn_wire_profile_get_descriptor(wire_profile);
    payload_length = path_revoke_payload_size(wire_profile);
    if (descriptor == NULL || payload_length > sizeof(payload)) {
        return UCN_ERR_CONFIG;
    }
    write_uint_be(payload, descriptor->path_id_bytes, path_id);
    write_uint_be(payload + descriptor->path_id_bytes,
                  descriptor->address_bytes, destination);
    result = send_control_to_node_profile(
        node, control_target, UCN_MSG_PATH_REVOKE, payload,
        (uint16_t)payload_length, wire_profile);
    if (result == UCN_OK) {
        node->stats.path_revokes_sent++;
    }
    return result;
}

/*
 * EN: Searches bounded Lite/Full Node state for `path_forward`.
 * 中文：在固定容量的 Lite/Full Node 状态中查找 `path_forward`。
 */
const ucn_path_forward_entry_t *ucn_node_find_path_forward(
    const ucn_node_t *node,
    ucn_node_id_t owner,
    ucn_session_id_t owner_session_id,
    ucn_path_id_t path_id,
    ucn_node_id_t destination)
{
    if (node == NULL) {
        return NULL;
    }
    return find_active_path(node, owner, owner_session_id, path_id, destination);
}
#endif

/*
 * EN: Validates and submits one application message to the Node transmit and routing pipeline.
 * 中文：验证一个应用消息，并将其提交到 Node 发送与路由流水线。
 */
ucn_result_t ucn_node_send(ucn_node_t *node,
                           ucn_node_id_t destination,
                           uint8_t message_type,
                           ucn_traffic_class_t traffic_class,
                           const uint8_t *payload,
                           uint16_t payload_length)
{
    ucn_frame_t frame;
    ucn_link_t *link;
    ucn_route_entry_t *route;
    ucn_link_status_t status;
    ucn_result_t result;
    uint8_t ciphertext[UCN_MAX_PAYLOAD_BYTES];
    uint8_t auth_tag[UCN_E2E_TAG_SIZE];

    if (node == NULL || destination == 0U ||
        (payload_length != 0U && payload == NULL)) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_node_security_ready(node)) {
        return UCN_ERR_SECURITY;
    }

    if ((uint32_t)traffic_class >= (uint32_t)UCN_TRAFFIC_CLASS_COUNT) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (ucn_message_type_is_control(message_type)) {
        return UCN_ERR_ARGUMENT;
    }

    link = find_link(node, destination);
    if (link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }

    result = get_link_status(link, &status);
    if (result != UCN_OK) {
        return result;
    }

    if (!status.is_up) {
        return UCN_ERR_LINK_DOWN;
    }

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = message_type;
    frame.traffic_class = traffic_class;
    frame.hop_limit = node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = destination;
    frame.session_id = node->session_id;
    route = find_active_route(node, destination);
    if (node->automatic_wire_profile) {
        frame.hop_limit = route != NULL && route->hop_count != 0U ?
                              route->hop_count : 1U;
    }
    if (route != NULL &&
        link == resolve_egress_link(node, route->egress_link) &&
        route->route_epoch != 0U) {
        frame.flags |= UCN_FRAME_FLAG_ROUTE_EXTENSION;
        frame.has_route_extension = true;
        frame.route_epoch = route->route_epoch;
    }
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        return result;
    }
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = payload_length;
    result = protect_outbound_business(
        node, link, &status, UCN_WIRE_PROFILE_UNSPECIFIED, 0U, &frame,
        ciphertext, auth_tag);
    if (result != UCN_OK) {
        return result;
    }
    return send_frame_on_logical_egress(node, link, &frame, &link);
}

/* Re-resolve a Path only after a definitive physical-Bearer failure.  The
 * first send returned LINK_DOWN, so retrying the unchanged frame once on the
 * newly selected Bearer cannot create a second successful delivery. */
#if UCN_FEATURE_PATH
/*
 * EN: Validates `frame_for_path_capability` before Lite/Full Node state is used or changed.
 * 中文：在使用或修改 Lite/Full Node 状态前验证 `frame_for_path_capability`。
 */
static ucn_result_t validate_frame_for_path_capability(
    ucn_node_t *node,
    const ucn_path_forward_entry_t *path,
    const ucn_frame_t *frame)
{
    ucn_path_effective_capability_t capability;
    size_t encoded_size;
    ucn_result_t result;

    result = resolve_path_effective_capability(node, path, &capability);
    if (result != UCN_OK) {
        return result;
    }
    if (frame->wire_profile > capability.maximum_wire_profile) {
        return UCN_ERR_UNSUPPORTED;
    }
    encoded_size = ucn_frame_encoded_size(frame);
    return encoded_size != 0U && encoded_size <= capability.minimum_mtu ?
        UCN_OK : UCN_ERR_TOO_LARGE;
}

/*
 * EN: Validates and submits `send_frame_on_path_egress` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_frame_on_path_egress` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_frame_on_path_egress(
    ucn_node_t *node,
    const ucn_path_forward_entry_t *path,
    const ucn_frame_t *frame,
    ucn_link_t **last_egress_link)
{
    ucn_link_t *configured_egress_link;
    ucn_link_t *link;
    ucn_result_t result;

    if (last_egress_link != NULL) {
        *last_egress_link = NULL;
    }
    if (node == NULL || path == NULL || path->egress_link == NULL || frame == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = validate_frame_for_path_capability(node, path, frame);
    if (result != UCN_OK) {
        return result;
    }
    configured_egress_link = path->egress_link;
    link = resolve_egress_link(node, configured_egress_link);
    if (link == NULL) {
        revoke_paths_by_unavailable_egress(node, configured_egress_link);
        return UCN_ERR_LINK_DOWN;
    }
    if (last_egress_link != NULL) {
        *last_egress_link = link;
    }
    result = send_frame_on_link(node, link, frame);
    if (result != UCN_ERR_LINK_DOWN) {
        return result;
    }

    link = resolve_egress_link(node, configured_egress_link);
    if (link == NULL || (last_egress_link != NULL && link == *last_egress_link)) {
        revoke_paths_by_unavailable_egress(node, configured_egress_link);
        return UCN_ERR_LINK_DOWN;
    }
    if (last_egress_link != NULL) {
        *last_egress_link = link;
    }
    result = send_frame_on_link(node, link, frame);
    if (result == UCN_ERR_LINK_DOWN) {
        revoke_paths_by_unavailable_egress(node, configured_egress_link);
    }
    return result;
}

/*
 * EN: Validates and submits `send_path` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_path` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
ucn_result_t ucn_node_send_path(ucn_node_t *node,
                                ucn_node_id_t destination,
                                uint8_t message_type,
                                ucn_traffic_class_t traffic_class,
                                ucn_path_id_t path_id,
                                const uint8_t *payload,
                                uint16_t payload_length)
{
    const ucn_path_forward_entry_t *path;
    ucn_path_effective_capability_t capability;
    ucn_frame_t frame;
    ucn_link_t *link;
    ucn_link_status_t status;
    ucn_result_t result;
    uint8_t ciphertext[UCN_MAX_PAYLOAD_BYTES];
    uint8_t auth_tag[UCN_E2E_TAG_SIZE];

    if (node == NULL || destination == 0U || path_id == 0U ||
        (payload_length != 0U && payload == NULL)) {
        return UCN_ERR_ARGUMENT;
    }
    if ((uint32_t)traffic_class >= (uint32_t)UCN_TRAFFIC_CLASS_COUNT) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (ucn_message_type_is_control(message_type)) {
        return UCN_ERR_ARGUMENT;
    }
    if (node->security_ops == NULL || node->session_id == 0U) {
        return UCN_ERR_SECURITY;
    }

    path = find_active_path(node, node->config.node_id, node->session_id,
                            path_id, destination);
    if (path == NULL || path->terminal || path->egress_link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    link = resolve_egress_link(node, path->egress_link);
    if (link == NULL) {
        return UCN_ERR_LINK_DOWN;
    }

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = message_type;
    frame.traffic_class = traffic_class;
    frame.flags = UCN_FRAME_FLAG_ROUTE_EXTENSION | UCN_FRAME_FLAG_PATH_ID;
    frame.hop_limit = path->remaining_hops;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = destination;
    frame.session_id = node->session_id;
    frame.has_route_extension = true;
    frame.has_path_id = true;
    frame.path_id = path_id;
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        return result;
    }
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = payload_length;
    result = get_link_status(link, &status);
    if (result != UCN_OK) {
        return result;
    }
    if (!status.is_up) {
        return UCN_ERR_LINK_DOWN;
    }
    result = resolve_path_effective_capability(node, path, &capability);
    if (result != UCN_OK) {
        return result;
    }
    result = protect_outbound_business(
        node, link, &status, capability.maximum_wire_profile,
        capability.minimum_mtu, &frame, ciphertext, auth_tag);
    if (result != UCN_OK) {
        if (path_result_is_capability_failure(result)) {
            node->stats.path_capability_failures++;
            revoke_path_and_mark_local_policy(node, node->config.node_id,
                                               node->session_id, path_id,
                                               destination);
        }
        return result;
    }
    result = send_frame_on_path_egress(node, path, &frame, &link);
    if (result == UCN_ERR_LINK_DOWN || path_result_is_capability_failure(result)) {
        if (path_result_is_capability_failure(result)) {
            node->stats.path_capability_failures++;
        }
        revoke_path_and_mark_local_policy(node, node->config.node_id,
                                           node->session_id, path_id,
                                           destination);
    }
    return result;
}
#endif

#if UCN_FEATURE_POLICY
/*
 * EN: Selects or resolves `resolve_policy_path_active_egress` using deterministic Lite/Full Node rules.
 * 中文：按照确定性的 Lite/Full Node 规则选择或解析 `resolve_policy_path_active_egress`。
 */
static ucn_link_t *resolve_policy_path_active_egress(
    ucn_node_t *node,
    const ucn_policy_path_entry_t *policy_path,
    const ucn_path_forward_entry_t **wire_path_out)
{
    const ucn_path_forward_entry_t *wire_path = NULL;

    if (wire_path_out != NULL) {
        *wire_path_out = NULL;
    }
    if (node == NULL || policy_path == NULL || policy_path->egress_link == NULL) {
        return NULL;
    }
    if (policy_path->wire_path_id != 0U) {
        wire_path = ucn_path_find(&node->path_state, node->config.node_id,
                                  node->session_id,
                                  policy_path->wire_path_id,
                                  policy_path->destination);
        if (wire_path == NULL || wire_path->terminal ||
            ucn_path_is_expired(wire_path, node->now_ms) ||
            wire_path->egress_link != policy_path->egress_link) {
            return NULL;
        }
    }
    if (wire_path_out != NULL) {
        *wire_path_out = wire_path;
    }
    return resolve_egress_link(node, policy_path->egress_link);
}

/*
 * EN: Updates `refresh_policy_path_bearers` in bounded Lite/Full Node state.
 * 中文：更新固定容量 Lite/Full Node 状态中的 `refresh_policy_path_bearers`。
 */
static void refresh_policy_path_bearers(ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_POLICY_PATHS; ++index) {
        ucn_policy_path_entry_t *path = &node->policy_state.paths[index];
        ucn_link_t *active_egress;

        if (!path->occupied) {
            continue;
        }
        active_egress = resolve_policy_path_active_egress(node, path, NULL);
        ucn_policy_refresh_path_egress(&node->policy_state,
                                       path->local_path_id,
                                       active_egress,
                                       active_egress != NULL);
    }
}

/*
 * EN: Checks the `pinned_path_has_hard_failure` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `pinned_path_has_hard_failure` 条件。
 */
static bool pinned_path_has_hard_failure(ucn_result_t result)
{
    /* These two results mean the selected authenticated Path no longer has a
     * usable local forwarding entry.  Backpressure, security failure and
     * generic driver errors must not silently move a deterministic flow. */
    return result == UCN_ERR_LINK_DOWN || result == UCN_ERR_NOT_FOUND ||
           path_result_is_capability_failure(result);
}
#endif

/*
 * EN: Selects or resolves `resolve_route_constraints` using deterministic Lite/Full Node rules.
 * 中文：按照确定性的 Lite/Full Node 规则选择或解析 `resolve_route_constraints`。
 */
static void resolve_route_constraints(
    const ucn_node_t *node,
    const ucn_route_constraints_t *specific,
    ucn_route_constraints_t *resolved)
{
    *resolved = node->default_route_constraints;
    if (specific == NULL) {
        return;
    }
    if (specific->max_hops != 0U) {
        resolved->max_hops = specific->max_hops;
    }
    if (specific->max_route_cost != 0U) {
        resolved->max_route_cost = specific->max_route_cost;
    }
    if (specific->max_verified_rtt_ms != 0U) {
        resolved->max_verified_rtt_ms = specific->max_verified_rtt_ms;
    }
    resolved->require_verified_rtt =
        resolved->require_verified_rtt || specific->require_verified_rtt;
}

/*
 * EN: Checks the `route_quality_meets_constraints` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `route_quality_meets_constraints` 条件。
 */
static bool route_quality_meets_constraints(
    const ucn_route_quality_t *quality,
    const ucn_route_constraints_t *constraints)
{
    if (quality == NULL || constraints == NULL || !quality->available ||
        quality->hop_count == 0U ||
        quality->hop_count > constraints->max_hops) {
        return false;
    }
    if (quality->route_cost == UCN_ROUTE_COST_UNKNOWN) {
        if (constraints->max_route_cost != UCN_ROUTE_COST_MAX) {
            return false;
        }
    } else if (quality->route_cost > constraints->max_route_cost) {
        return false;
    }
    if ((constraints->require_verified_rtt ||
         constraints->max_verified_rtt_ms != 0U) &&
        !quality->verified_rtt_valid) {
        return false;
    }
    return constraints->max_verified_rtt_ms == 0U ||
           quality->verified_rtt_ms <= constraints->max_verified_rtt_ms;
}

/*
 * EN: Validates and submits `send_endpoint_auto_best` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_endpoint_auto_best` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_endpoint_auto_best(
                                             ucn_node_t *node,
                                             ucn_node_id_t destination,
                                             ucn_endpoint_t endpoint,
                                             ucn_traffic_class_t traffic_class,
                                             const ucn_route_constraints_t *specific,
                                             const uint8_t *payload,
                                             uint16_t payload_length,
                                             bool allow_pending_queue)
{
    ucn_route_constraints_t constraints;
    ucn_route_quality_t quality;
    ucn_route_entry_t *active_route;
    bool route_usable;
    ucn_result_t result;

    resolve_route_constraints(node, specific, &constraints);
    result = ucn_node_get_route_quality(node, destination, &quality);
    route_usable = result == UCN_OK &&
                   route_quality_meets_constraints(&quality, &constraints);
    if (traffic_class == UCN_TRAFFIC_Q1_REALTIME && !route_usable) {
        active_route = find_active_route(node, destination);
        result = active_route != NULL && !active_route->is_static ?
                     begin_route_discovery(node, destination, node->now_ms, true,
                                           constraints.max_hops, false,
                                           constraints.require_verified_rtt ||
                                               constraints.max_verified_rtt_ms != 0U) :
                     begin_route_discovery(node, destination, node->now_ms, false,
                                           constraints.max_hops, false, false);
        if (result != UCN_OK) {
            return result;
        }
        result = ucn_node_get_route_quality(node, destination, &quality);
        route_usable = result == UCN_OK &&
                       route_quality_meets_constraints(&quality, &constraints);
        if (!route_usable) {
            if (!allow_pending_queue) {
                return UCN_ERR_NOT_FOUND;
            }
            return queue_pending_q1(node, destination, (uint8_t)endpoint, payload,
                                    payload_length);
        }
    }
    if (!route_usable) {
        return UCN_ERR_NOT_FOUND;
    }
    return ucn_node_send(node, destination, (uint8_t)endpoint, traffic_class,
                         payload, payload_length);
}

#if UCN_FEATURE_POLICY
/*
 * EN: Checks the `policy_path_meets_constraints` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `policy_path_meets_constraints` 条件。
 */
static bool policy_path_meets_constraints(
    const ucn_node_t *node,
    const ucn_policy_path_entry_t *policy_path,
    const ucn_path_forward_entry_t *wire_path,
    const ucn_route_constraints_t *specific)
{
    ucn_route_constraints_t constraints;

    resolve_route_constraints(node, specific, &constraints);
    if (policy_path == NULL || wire_path == NULL || wire_path->terminal ||
        wire_path->remaining_hops == 0U ||
        wire_path->remaining_hops > constraints.max_hops) {
        return false;
    }
    if (constraints.max_route_cost != UCN_ROUTE_COST_MAX &&
        (!policy_path->route_cost_valid ||
         policy_path->route_cost > constraints.max_route_cost)) {
        return false;
    }
    if ((constraints.require_verified_rtt ||
         constraints.max_verified_rtt_ms != 0U) &&
        !policy_path->verified_rtt_valid) {
        return false;
    }
    return constraints.max_verified_rtt_ms == 0U ||
           policy_path->verified_rtt_ms <= constraints.max_verified_rtt_ms;
}

/*
 * EN: Validates and submits `send_endpoint_on_policy_path` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_endpoint_on_policy_path` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_endpoint_on_policy_path(
    ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class,
    uint16_t local_path_id,
    const ucn_route_constraints_t *constraints,
    const uint8_t *payload,
    uint16_t payload_length)
{
    const ucn_policy_path_entry_t *path;
    const ucn_path_forward_entry_t *wire_path;
    ucn_result_t result;

    path = ucn_node_find_policy_path(node, local_path_id);
    if (path == NULL || path->destination != destination ||
        path->wire_path_id == 0U || path->state == UCN_POLICY_PATH_EMPTY ||
        path->state == UCN_POLICY_PATH_CANDIDATE) {
        node->policy_state.stats.pinned_policy_config_errors++;
        return UCN_ERR_CONFIG;
    }
    if (path->state == UCN_POLICY_PATH_DOWN) {
        return UCN_ERR_LINK_DOWN;
    }
    wire_path = ucn_path_find(&node->path_state, node->config.node_id,
                              node->session_id, path->wire_path_id,
                              destination);
    if (!policy_path_meets_constraints(node, path, wire_path, constraints)) {
        return UCN_ERR_NOT_FOUND;
    }

    result = ucn_node_send_path(node, destination, (uint8_t)endpoint,
                                traffic_class, path->wire_path_id, payload,
                                payload_length);
    if (pinned_path_has_hard_failure(result)) {
        ucn_policy_mark_path_down(&node->policy_state, local_path_id);
    }
    return result;
}

/*
 * EN: Checks the `auto_balance_path_is_member` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `auto_balance_path_is_member` 条件。
 */
static bool auto_balance_path_is_member(const ucn_route_policy_config_t *config,
                                        uint16_t local_path_id)
{
    return local_path_id != 0U &&
           (local_path_id == config->primary_local_path_id ||
            local_path_id == config->backup_local_path_id);
}

/*
 * EN: Checks the `auto_balance_path_is_usable` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `auto_balance_path_is_usable` 条件。
 */
static bool auto_balance_path_is_usable(const ucn_node_t *node,
                                         const ucn_route_policy_config_t *config,
                                         ucn_node_id_t destination,
                                        uint16_t local_path_id)
{
    const ucn_policy_path_entry_t *path;
    const ucn_path_forward_entry_t *wire_path;
    const ucn_policy_link_quality_snapshot_t *quality;
    ucn_link_t *active_egress;

    path = ucn_node_find_policy_path(node, local_path_id);
    if (path == NULL || path->destination != destination ||
        path->wire_path_id == 0U || path->state != UCN_POLICY_PATH_VERIFIED) {
        return false;
    }
    active_egress = resolve_policy_path_active_egress((ucn_node_t *)node, path,
                                                       &wire_path);
    if (active_egress == NULL) {
        return false;
    }
    quality = ucn_node_get_link_quality(node, active_egress);
    if (quality != NULL && !quality->cost.selectable) {
        return false;
    }
    return wire_path != NULL &&
           policy_path_meets_constraints(node, path, wire_path,
                                           &config->constraints);
}

/*
 * EN: Checks the `auto_balance_path_is_congested` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `auto_balance_path_is_congested` 条件。
 */
static bool auto_balance_path_is_congested(const ucn_node_t *node,
                                           uint16_t local_path_id)
{
    const ucn_policy_path_entry_t *path =
        ucn_node_find_policy_path(node, local_path_id);

    return path != NULL && path->congestion_samples >=
                               UCN_POLICY_BALANCE_CONGESTED_SAMPLE_LIMIT;
}

/*
 * EN: Calculates the bounded `auto_balance_active_flow_count` value used by Lite/Full Node.
 * 中文：计算 Lite/Full Node 使用的有界 `auto_balance_active_flow_count` 值。
 */
static size_t auto_balance_active_flow_count(const ucn_node_t *node,
                                             uint16_t local_path_id)
{
    size_t index;
    size_t count = 0U;

    for (index = 0U; index < UCN_MAX_POLICY_FLOWS; ++index) {
        const ucn_policy_flow_binding_t *flow = &node->policy_state.flows[index];

        if (flow->occupied && flow->local_path_id == local_path_id &&
            !ucn_deadline_expired(node->now_ms, flow->expires_at_ms)) {
            count++;
        }
    }
    return count;
}

/*
 * EN: Calculates `auto_balance_path_score` with bounded, deterministic Lite/Full Node arithmetic.
 * 中文：使用有界且确定性的 Lite/Full Node 算术计算 `auto_balance_path_score`。
 */
static uint32_t auto_balance_path_score(const ucn_node_t *node,
                                        uint16_t local_path_id)
{
    const ucn_policy_path_entry_t *path =
        ucn_node_find_policy_path(node, local_path_id);
    ucn_link_t *active_egress;
    const uint32_t unknown_score_base = UINT32_MAX / 2U;
    uint16_t effective_cost;
    bool effective_cost_known;
    uint32_t score;
    size_t active_flows;

    if (path == NULL) {
        return UINT32_MAX;
    }
    active_egress = resolve_policy_path_active_egress((ucn_node_t *)node,
                                                       path, NULL);
    active_flows = auto_balance_active_flow_count(node, local_path_id);
    if (active_egress == NULL ||
        !link_local_select_cost((ucn_node_t *)node, active_egress,
                                &effective_cost_known, &effective_cost)) {
        return UINT32_MAX;
    }
    if (!effective_cost_known) {
        return active_flows >= (size_t)(UINT32_MAX - unknown_score_base) ?
                   UINT32_MAX : unknown_score_base + (uint32_t)active_flows;
    }
    {
        const uint64_t weighted = (uint64_t)effective_cost *
                                  (uint64_t)(active_flows + 1U);

        score = weighted >= unknown_score_base ? unknown_score_base - 1U :
                                                 (uint32_t)weighted;
    }
    return score;
}

/*
 * EN: Checks the `auto_balance_has_configured_path` condition against current Lite/Full Node state.
 * 中文：根据当前 Lite/Full Node 状态检查 `auto_balance_has_configured_path` 条件。
 */
static bool auto_balance_has_configured_path(
    const ucn_node_t *node,
    const ucn_route_policy_config_t *config,
    ucn_node_id_t destination)
{
    const ucn_policy_path_entry_t *path;

    path = ucn_node_find_policy_path(node, config->primary_local_path_id);
    if (path != NULL && path->destination == destination) {
        return true;
    }
    path = ucn_node_find_policy_path(node, config->backup_local_path_id);
    return path != NULL && path->destination == destination;
}

/*
 * EN: Selects an eligible Path from the bounded automatic-balance candidate set.
 * 中文：从固定容量的自动均衡候选集中选择合格 Path。
 */
static uint16_t auto_balance_select_path(const ucn_node_t *node,
                                         const ucn_route_policy_config_t *config,
                                         ucn_node_id_t destination,
                                         uint16_t excluded_local_path_id)
{
    const uint16_t candidates[2] = {
        config->primary_local_path_id,
        config->backup_local_path_id
    };
    uint16_t selected = 0U;
    uint32_t best_score = UINT32_MAX;
    bool has_noncongested = false;
    size_t index;

    for (index = 0U; index < 2U; ++index) {
        const uint16_t candidate = candidates[index];

        if (candidate != excluded_local_path_id &&
            auto_balance_path_is_usable(node, config, destination, candidate) &&
            !auto_balance_path_is_congested(node, candidate)) {
            has_noncongested = true;
            break;
        }
    }
    for (index = 0U; index < 2U; ++index) {
        const uint16_t candidate = candidates[index];
        uint32_t score;

        if (candidate == excluded_local_path_id ||
            !auto_balance_path_is_usable(node, config, destination, candidate) ||
            (has_noncongested && auto_balance_path_is_congested(node, candidate))) {
            continue;
        }
        score = auto_balance_path_score(node, candidate);
        if (selected == 0U || score < best_score ||
            (score == best_score && candidate < selected)) {
            selected = candidate;
            best_score = score;
        }
    }
    return selected;
}

/*
 * EN: Validates and installs `auto_balance_bind_path` in bounded Lite/Full Node state.
 * 中文：验证 `auto_balance_bind_path` 并将其安装到固定容量的 Lite/Full Node 状态中。
 */
static ucn_result_t auto_balance_bind_path(ucn_node_t *node,
                                           ucn_node_id_t destination,
                                           ucn_endpoint_t endpoint,
                                           uint16_t local_path_id,
                                           uint32_t lease_ms)
{
    return ucn_node_bind_q1_flow(node, destination, endpoint, local_path_id,
                                 lease_ms);
}

/*
 * EN: Validates and submits `send_endpoint_auto_balance` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_endpoint_auto_balance` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_endpoint_auto_balance(
    ucn_node_t *node,
    const ucn_route_policy_entry_t *policy,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    const uint8_t *payload,
    uint16_t payload_length)
{
    const ucn_route_policy_config_t *config = &policy->config;
    const ucn_policy_flow_binding_t *flow;
    const uint32_t lease_ms = config->balance_flow_lease_ms == 0U ?
                                  UCN_POLICY_BALANCE_FLOW_LEASE_MS :
                                  config->balance_flow_lease_ms;
    uint16_t selected_local_path_id;
    bool rebinding = false;
    bool congestion_rebind = false;
    bool down_rebind = false;
    ucn_result_t result;

    flow = ucn_node_find_q1_flow(node, destination, endpoint);
    if (flow != NULL && auto_balance_path_is_member(config, flow->local_path_id) &&
        auto_balance_path_is_usable(node, config, destination,
                                    flow->local_path_id) &&
        !auto_balance_path_is_congested(node, flow->local_path_id)) {
        selected_local_path_id = flow->local_path_id;
    } else {
        rebinding = flow != NULL;
        congestion_rebind = rebinding &&
                             auto_balance_path_is_member(config,
                                                         flow->local_path_id) &&
                             auto_balance_path_is_usable(node, config, destination,
                                                         flow->local_path_id) &&
                             auto_balance_path_is_congested(node,
                                                            flow->local_path_id);
        down_rebind = rebinding &&
                      auto_balance_path_is_member(config, flow->local_path_id) &&
                      !auto_balance_path_is_usable(node, config, destination,
                                                    flow->local_path_id);
        selected_local_path_id = auto_balance_select_path(node, config,
                                                           destination, 0U);
        if (selected_local_path_id == 0U) {
            node->policy_state.stats.auto_balance_selection_failures++;
            return auto_balance_has_configured_path(node, config, destination) ?
                       UCN_ERR_LINK_DOWN : UCN_ERR_CONFIG;
        }
        result = auto_balance_bind_path(node, destination, endpoint,
                                        selected_local_path_id, lease_ms);
        if (result != UCN_OK) {
            node->policy_state.stats.auto_balance_selection_failures++;
            return result;
        }
        if (rebinding) {
            node->policy_state.stats.auto_balance_rebindings++;
            if (congestion_rebind) {
                node->policy_state.stats.auto_balance_congestion_rebindings++;
            } else if (down_rebind) {
                node->policy_state.stats.auto_balance_down_rebindings++;
            }
        } else {
            node->policy_state.stats.auto_balance_flow_bindings++;
        }
    }

    result = send_endpoint_on_policy_path(node, destination, endpoint,
                                          UCN_TRAFFIC_Q1_REALTIME,
                                          selected_local_path_id,
                                          &config->constraints, payload,
                                          payload_length);
    if (result == UCN_OK) {
        node->policy_state.stats.auto_balance_sends++;
        ucn_policy_touch_q1_flow(&node->policy_state, destination, endpoint,
                                 node->now_ms);
        return UCN_OK;
    }
    if (!pinned_path_has_hard_failure(result)) {
        return result;
    }

    /* The selected Path was proven down by this send.  Rebind once and retry
     * only on another verified member; this is failover, never replication. */
    selected_local_path_id = auto_balance_select_path(node, config, destination,
                                                       selected_local_path_id);
    if (selected_local_path_id == 0U) {
        node->policy_state.stats.auto_balance_selection_failures++;
        return result;
    }
    result = auto_balance_bind_path(node, destination, endpoint,
                                    selected_local_path_id, lease_ms);
    if (result != UCN_OK) {
        node->policy_state.stats.auto_balance_selection_failures++;
        return result;
    }
    node->policy_state.stats.auto_balance_rebindings++;
    node->policy_state.stats.auto_balance_down_rebindings++;
    result = send_endpoint_on_policy_path(node, destination, endpoint,
                                          UCN_TRAFFIC_Q1_REALTIME,
                                          selected_local_path_id,
                                          &config->constraints, payload,
                                          payload_length);
    if (result == UCN_OK) {
        node->policy_state.stats.auto_balance_sends++;
        ucn_policy_touch_q1_flow(&node->policy_state, destination, endpoint,
                                 node->now_ms);
    }
    return result;
}

/*
 * EN: Validates and submits `send_endpoint_pinned` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_endpoint_pinned` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_endpoint_pinned(
    ucn_node_t *node,
    const ucn_route_policy_entry_t *policy,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class,
    const uint8_t *payload,
    uint16_t payload_length,
    bool allow_pending_queue)
{
    const ucn_route_policy_config_t *config = &policy->config;
    ucn_result_t result;

    if (config->primary_local_path_id == 0U) {
        node->policy_state.stats.pinned_policy_config_errors++;
        return UCN_ERR_CONFIG;
    }

    result = send_endpoint_on_policy_path(node, destination, endpoint,
                                          traffic_class,
                                          config->primary_local_path_id,
                                          &config->constraints, payload,
                                          payload_length);
    if (config->mode == UCN_ROUTE_POLICY_PINNED_STRICT) {
        if (result == UCN_OK) {
            node->policy_state.stats.pinned_strict_sends++;
        } else {
            node->policy_state.stats.pinned_strict_failures++;
        }
        return result;
    }

    if (result == UCN_OK) {
        node->policy_state.stats.pinned_failover_primary_sends++;
        return UCN_OK;
    }
    if (!pinned_path_has_hard_failure(result)) {
        return result;
    }
    node->policy_state.stats.pinned_failover_hard_failures++;

    if (config->backup_local_path_id != 0U) {
        result = send_endpoint_on_policy_path(node, destination, endpoint,
                                              traffic_class,
                                              config->backup_local_path_id,
                                              &config->constraints, payload,
                                              payload_length);
        if (result == UCN_OK) {
            node->policy_state.stats.pinned_failover_backup_sends++;
            return UCN_OK;
        }
        if (!pinned_path_has_hard_failure(result)) {
            return result;
        }
    }

    /* Discovery is a deliberate last resort of PINNED_FAILOVER.  Q0 never
     * waits for it, and strict mode never reaches this branch. */
    if (config->allow_discovery_on_hard_failure &&
        traffic_class == UCN_TRAFFIC_Q1_REALTIME) {
        node->policy_state.stats.pinned_failover_discovery_fallbacks++;
        return send_endpoint_auto_best(node, destination, endpoint,
                                       traffic_class, &config->constraints,
                                       payload, payload_length,
                                       allow_pending_queue);
    }
    return result;
}
#endif

/*
 * EN: Validates and submits `send_endpoint_internal` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_endpoint_internal` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_endpoint_internal(
    ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class,
    const uint8_t *payload,
    uint16_t payload_length,
    bool allow_pending_queue)
{
    const ucn_route_constraints_t *route_constraints = NULL;
#if UCN_FEATURE_POLICY
    const ucn_route_policy_entry_t *policy;
#endif

    if (node == NULL || !ucn_endpoint_is_static(endpoint) || destination == 0U ||
        (payload_length != 0U && payload == NULL)) {
        return UCN_ERR_ARGUMENT;
    }
    /* Reject the enum in its original width before Policy converts its key to
     * uint8_t.  Otherwise values such as -255/-256 can alias Q1/Q0 and reach a
     * matching AUTO_BALANCE or wildcard rule. */
    if ((uint32_t)traffic_class >= (uint32_t)UCN_TRAFFIC_CLASS_COUNT) {
        return UCN_ERR_UNSUPPORTED;
    }
#if UCN_FEATURE_POLICY
    policy = ucn_node_find_route_policy(node, destination, endpoint, traffic_class);
    if (policy != NULL) {
        size_t policy_index;

        node->policy_state.stats.policy_match_hits++;
        for (policy_index = 0U; policy_index < UCN_MAX_ROUTE_POLICIES;
             ++policy_index) {
            if (&node->policy_state.policies[policy_index] == policy) {
                node->policy_state.policies[policy_index].match_hits++;
                break;
            }
        }
        if (policy->config.mode == UCN_ROUTE_POLICY_PINNED_STRICT ||
            policy->config.mode == UCN_ROUTE_POLICY_PINNED_FAILOVER) {
            return send_endpoint_pinned(node, policy, destination, endpoint,
                                        traffic_class, payload, payload_length,
                                        allow_pending_queue);
        }
        if (policy->config.mode == UCN_ROUTE_POLICY_AUTO_BALANCE) {
            return send_endpoint_auto_balance(node, policy, destination, endpoint,
                                              payload, payload_length);
        }
        route_constraints = &policy->config.constraints;
    }
#endif
    return send_endpoint_auto_best(node, destination, endpoint, traffic_class,
                                   route_constraints, payload, payload_length,
                                   allow_pending_queue);
}

/*
 * EN: Validates and submits `send_endpoint` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_endpoint` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
ucn_result_t ucn_node_send_endpoint(ucn_node_t *node,
                                    ucn_node_id_t destination,
                                    ucn_endpoint_t endpoint,
                                    ucn_traffic_class_t traffic_class,
                                    const uint8_t *payload,
                                    uint16_t payload_length)
{
    return send_endpoint_internal(node, destination, endpoint, traffic_class,
                                  payload, payload_length, true);
}

/*
 * EN: Copies `enqueue` into a bounded Lite/Full Node queue.
 * 中文：把 `enqueue` 复制到固定容量的 Lite/Full Node 队列。
 */
ucn_result_t ucn_node_enqueue(ucn_node_t *node,
                              const ucn_send_request_t *request)
{
    ucn_tx_item_t *items;
    ucn_tx_item_t *slot = NULL;
    size_t count;
    size_t index;

    if (node == NULL || request == NULL || request->destination == 0U ||
        (request->payload_length != 0U && request->payload == NULL)) {
        return UCN_ERR_ARGUMENT;
    }

    if ((uint32_t)request->traffic_class >=
        (uint32_t)UCN_TRAFFIC_CLASS_COUNT) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (ucn_message_type_is_control(request->message_type)) {
        return UCN_ERR_ARGUMENT;
    }

    if (request->delivery != UCN_DELIVERY_BEST_EFFORT &&
        request->delivery != UCN_DELIVERY_LATEST_VALUE &&
        request->delivery != UCN_DELIVERY_RETRY_ON_BACKPRESSURE) {
        return UCN_ERR_ARGUMENT;
    }
    if (request->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE &&
        (request->traffic_class != UCN_TRAFFIC_Q0_CRITICAL ||
         request->deadline_ms == 0U)) {
        return UCN_ERR_ARGUMENT;
    }
    if (request->delivery == UCN_DELIVERY_LATEST_VALUE &&
        request->traffic_class != UCN_TRAFFIC_Q1_REALTIME) {
        return UCN_ERR_ARGUMENT;
    }

    if (request->payload_length > UCN_MAX_PAYLOAD_BYTES) {
        return UCN_ERR_TOO_LARGE;
    }

    items = queue_items(node, request->traffic_class, &count);
    if (request->delivery == UCN_DELIVERY_LATEST_VALUE) {
        for (index = 0U; index < count; ++index) {
            if (items[index].occupied &&
                items[index].destination == request->destination &&
                items[index].message_type == request->message_type) {
                slot = &items[index];
                break;
            }
        }
    }

    if (slot == NULL) {
        for (index = 0U; index < count; ++index) {
            if (!items[index].occupied) {
                slot = &items[index];
                break;
            }
        }
    }

    if (slot == NULL) {
        return UCN_ERR_NO_SPACE;
    }

    slot->occupied = true;
    slot->destination = request->destination;
    slot->message_type = request->message_type;
    slot->traffic_class = request->traffic_class;
    slot->delivery = request->delivery;
    slot->deadline_ms = request->deadline_ms;
    slot->next_attempt_ms = 0U;
    slot->order = node->next_queue_order++;
    slot->backpressure_retries = 0U;
    slot->waiting_for_route = false;
    slot->payload_length = request->payload_length;
    if (request->payload_length != 0U) {
        (void)memcpy(slot->payload, request->payload, request->payload_length);
    }
    node->stats.tx_enqueued_by_class[(uint8_t)request->traffic_class]++;
    return UCN_OK;
}

#if UCN_FEATURE_DIAGNOSTICS
/*
 * EN: Builds and submits `request_path_trace` through the bounded Lite/Full Node transmit path.
 * 中文：构造 `request_path_trace` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
ucn_result_t ucn_node_request_path_trace(ucn_node_t *node,
                                         ucn_node_id_t destination,
                                         uint8_t record_limit,
                                         ucn_path_trace_handler_t handler,
                                         void *context)
{
    ucn_wire_profile_t wire_profile;
    const ucn_wire_profile_descriptor_t *descriptor;
    ucn_path_trace_pending_t *pending;
    ucn_link_t *link;
    uint8_t payload[UCN_PATH_TRACE_MAX_PAYLOAD_BYTES];
    uint32_t trace_id;
    ucn_result_t result;

    if (node == NULL || handler == NULL || destination == 0U ||
        destination == UCN_NODE_BROADCAST || destination == node->config.node_id) {
        return UCN_ERR_ARGUMENT;
    }
    if (record_limit == 0U) {
        record_limit = (uint8_t)UCN_PATH_TRACE_MAX_NODES;
    }
    if (record_limit > UCN_PATH_TRACE_MAX_NODES) {
        return UCN_ERR_TOO_LARGE;
    }

    expire_path_trace_state(node, node->now_ms);
    pending = find_free_path_trace_pending(node);
    if (pending == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    link = find_link(node, destination);
    if (link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    result = select_path_trace_profile(node, link, destination, 1U,
                                       &wire_profile);
    if (result != UCN_OK) {
        return result;
    }
    descriptor = ucn_wire_profile_get_descriptor(wire_profile);
    if (descriptor == NULL) {
        return UCN_ERR_CONFIG;
    }
    if (!take_path_trace_token(node)) {
        return UCN_ERR_NO_SPACE;
    }

    trace_id = node->next_path_trace_id;
    if (trace_id == 0U || trace_id == UINT32_MAX) {
        trace_id = 1U;
    }
    node->next_path_trace_id = trace_id + 1U;
    (void)memset(pending, 0, sizeof(*pending));
    pending->occupied = true;
    pending->destination = destination;
    pending->trace_id = trace_id;
    pending->deadline_ms =
        ucn_deadline_from_now(node->now_ms, UCN_PATH_TRACE_TIMEOUT_MS);
    pending->handler = handler;
    pending->context = context;

    (void)memset(payload, 0, sizeof(payload));
    write_u32_be(payload + UCN_PATH_TRACE_TRACE_ID_OFFSET, trace_id);
    payload[UCN_PATH_TRACE_RECORD_COUNT_OFFSET] = 1U;
    payload[UCN_PATH_TRACE_RECORD_LIMIT_OFFSET] = record_limit;
    payload[UCN_PATH_TRACE_STATUS_OFFSET] = (uint8_t)UCN_PATH_TRACE_STATUS_OK;
    write_uint_be(payload + UCN_PATH_TRACE_NODE_IDS_OFFSET,
                  descriptor->address_bytes, node->config.node_id);
    result = send_path_trace_request_on_link(node, link, destination, payload,
                                             (uint16_t)path_trace_payload_size(
                                                 wire_profile, 1U),
                                             wire_profile);
    if (result != UCN_OK) {
        if (pending->occupied && pending->trace_id == trace_id) {
            pending->occupied = false;
        }
        return result;
    }
    node->stats.path_trace_requests_sent++;
    mark_route_used(node, destination);
    return UCN_OK;
}

/*
 * EN: Builds and submits `request_node_snapshot` through the bounded Lite/Full Node transmit path.
 * 中文：构造 `request_node_snapshot` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
ucn_result_t ucn_node_request_node_snapshot(
    ucn_node_t *node,
    uint8_t result_limit,
    ucn_node_snapshot_handler_t handler,
    void *context)
{
    ucn_node_snapshot_pending_t *pending;
    ucn_frame_t frame;
    uint8_t payload[UCN_NODE_SNAPSHOT_REQUEST_PAYLOAD_BYTES];
    uint32_t query_id;
    ucn_result_t result;

    if (node == NULL || handler == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (result_limit == 0U) {
        result_limit = (uint8_t)UCN_NODE_SNAPSHOT_MAX_RESULTS;
    }
    if (result_limit > UCN_NODE_SNAPSHOT_MAX_RESULTS) {
        return UCN_ERR_TOO_LARGE;
    }

    expire_node_snapshot_state(node, node->now_ms);
    pending = find_free_node_snapshot_pending(node);
    if (pending == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    if (!take_node_snapshot_token(node)) {
        return UCN_ERR_NO_SPACE;
    }

    query_id = node->next_node_snapshot_id;
    if (query_id == 0U || query_id == UINT32_MAX) {
        query_id = 1U;
    }
    node->next_node_snapshot_id = query_id + 1U;
    (void)memset(pending, 0, sizeof(*pending));
    pending->occupied = true;
    pending->result_limit = result_limit;
    pending->query_id = query_id;
    pending->deadline_ms =
        ucn_deadline_from_now(node->now_ms, UCN_NODE_SNAPSHOT_TIMEOUT_MS);
    pending->node_count = 1U;
    pending->entries[0].node_id = node->config.node_id;
    pending->entries[0].direct_link_count = (uint8_t)node->link_count;
    pending->handler = handler;
    pending->context = context;

    (void)memset(&frame, 0, sizeof(frame));
    (void)memset(payload, 0, sizeof(payload));
    write_u32_be(payload + UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET, query_id);
    frame.message_type = UCN_MSG_NODE_SNAPSHOT_REQ;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.flags = UCN_FRAME_FLAG_DIAGNOSTIC;
    frame.hop_limit = node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = UCN_NODE_BROADCAST;
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        pending->occupied = false;
        return result;
    }
    frame.session_id = node->session_id;

    /* The origin also remembers its own flood so a loop cannot cause it to
     * become a responder or re-flood the same Query ID. */
    result = ucn_duplicate_accept_frame(node, &frame);
    if (result != UCN_OK) {
        pending->occupied = false;
        return result;
    }
    result = forward_node_snapshot_request(node, NULL, &frame);
    if (result != UCN_OK) {
        pending->occupied = false;
        return result;
    }
    node->stats.node_snapshot_requests_sent++;
    return UCN_OK;
}

/*
 * EN: Builds and submits `request_policy_diagnostic` through the bounded Lite/Full Node transmit path.
 * 中文：构造 `request_policy_diagnostic` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
ucn_result_t ucn_node_request_policy_diagnostic(
    ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_policy_diagnostic_section_t section,
    uint8_t index,
    ucn_policy_diagnostic_handler_t handler,
    void *context)
{
    ucn_policy_diagnostic_pending_t *pending;
    uint32_t request_id;

    if (node == NULL || handler == NULL || destination == 0U ||
        destination == UCN_NODE_BROADCAST || destination == node->config.node_id ||
        !policy_diagnostic_selector_is_valid((uint8_t)section, index)) {
        return UCN_ERR_ARGUMENT;
    }
    expire_policy_diagnostic_state(node, node->now_ms);
    pending = find_free_policy_diagnostic_pending(node);
    if (pending == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    if (find_link(node, destination) == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    if (!take_policy_diagnostic_token(node)) {
        return UCN_ERR_NO_SPACE;
    }
    request_id = node->next_policy_diagnostic_id;
    if (request_id == 0U || request_id == UINT32_MAX) {
        request_id = 1U;
    }
    node->next_policy_diagnostic_id = request_id + 1U;
    (void)memset(pending, 0, sizeof(*pending));
    pending->occupied = true;
    pending->destination = destination;
    pending->request_id = request_id;
    pending->deadline_ms =
        ucn_deadline_from_now(node->now_ms, UCN_POLICY_DIAGNOSTIC_TIMEOUT_MS);
    pending->section = section;
    pending->index = index;
    pending->handler = handler;
    pending->context = context;
    /* `ucn_node_step()` sends this only after ordinary Q0-Q3 work. */
    return UCN_OK;
}
#endif

/*
 * EN: Records `note_business_transmission` in bounded Lite/Full Node state or statistics.
 * 中文：在固定容量的 Lite/Full Node 状态或统计中记录 `note_business_transmission`。
 */
static void note_business_transmission(ucn_node_t *node)
{
    if (node->business_tx_since_maintenance <
        UCN_BUSINESS_TX_BURST_BEFORE_MAINTENANCE) {
        node->business_tx_since_maintenance++;
    }
}

/* Only liveness and path/routing maintenance belongs here.  Snapshot and
 * policy diagnostics remain best-effort background work and must not take a
 * Q0-Q3 budget slot. */
/*
 * EN: Validates and submits `send_due_essential_maintenance` through the bounded Lite/Full Node transmit path.
 * 中文：验证 `send_due_essential_maintenance` 并将其提交到有界的 Lite/Full Node 发送路径。
 */
static ucn_result_t send_due_essential_maintenance(ucn_node_t *node,
                                                    uint32_t now_ms)
{
    ucn_result_t result;

    result = send_due_route_discovery_ring(node, now_ms);
    if (result != UCN_ERR_NOT_FOUND) {
        return result;
    }
#if UCN_TEST_NODE_MAINTENANCE_STAGE_LIMIT == 1
    return UCN_ERR_NOT_FOUND;
#endif
    result = send_due_heartbeat(node, now_ms);
    if (result != UCN_ERR_NOT_FOUND) {
        return result;
    }
#if UCN_TEST_NODE_MAINTENANCE_STAGE_LIMIT == 2
    return UCN_ERR_NOT_FOUND;
#endif
    result = send_due_bearer_quality_probe(node, now_ms);
    if (result != UCN_ERR_NOT_FOUND) {
        return result;
    }
#if UCN_TEST_NODE_MAINTENANCE_STAGE_LIMIT == 3
    return UCN_ERR_NOT_FOUND;
#endif
#if UCN_FEATURE_CANDIDATE_ROUTING
    result = send_due_path_probe(node, now_ms);
    if (result != UCN_ERR_NOT_FOUND) {
        return result;
    }
#if UCN_TEST_NODE_MAINTENANCE_STAGE_LIMIT == 4
    return UCN_ERR_NOT_FOUND;
#endif
    return start_due_route_refresh(node, now_ms);
#else
    return UCN_ERR_NOT_FOUND;
#endif
}

/*
 * EN: Updates `observe_step_interval` in bounded Lite/Full Node state.
 * 中文：更新固定容量 Lite/Full Node 状态中的 `observe_step_interval`。
 */
static void observe_step_interval(ucn_node_t *node, uint32_t now_ms)
{
    uint32_t gap_ms;

    if (!node->step_observation_started) {
        node->step_observation_started = true;
        node->stats.last_step_ms = now_ms;
        return;
    }

    gap_ms = (uint32_t)(now_ms - node->stats.last_step_ms);
    node->stats.last_step_ms = now_ms;
    if (gap_ms > node->stats.max_step_gap_ms) {
        node->stats.max_step_gap_ms = gap_ms;
    }
    if (gap_ms > UCN_MAX_STEP_INTERVAL_MS) {
        node->stats.step_interval_violations++;
    }
}

/*
 * EN: Advances one bounded Node maintenance and transmit scheduling cycle.
 * 中文：推进一次有界的 Node 维护与发送调度周期。
 */
ucn_result_t ucn_node_step(ucn_node_t *node, uint32_t now_ms)
{
    ucn_tx_item_t *item;
    uint8_t next_schedule_cursor;
    size_t count;
    uint32_t error_drops_before;
    ucn_result_t result;
#if UCN_FEATURE_DYNAMIC_MESH
    bool was_waiting_for_route;
    bool route_wait_failure = false;
#endif

    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_node_security_ready(node)) {
        return UCN_ERR_SECURITY;
    }

    /* Observe before any early return so every valid Protocol Task iteration
     * contributes to the product timing contract.  A violation is diagnostic:
     * stopping an already-late scheduler would only delay liveness further. */
    observe_step_interval(node, now_ms);
#if UCN_TEST_NODE_STEP_STAGE_LIMIT == 1
    return UCN_ERR_NOT_FOUND;
#endif

    expire_dynamic_state(node, now_ms);
#if UCN_TEST_NODE_STEP_STAGE_LIMIT == 2
    return UCN_ERR_NOT_FOUND;
#endif
#if UCN_FEATURE_POLICY && UCN_TEST_NODE_POLICY_REFRESH_ENABLED
    if (ucn_policy_refresh_link_quality(&node->policy_state, node->links,
                                        node->link_count, now_ms)) {
        refresh_policy_path_bearers(node);
    }
    ucn_policy_expire_flows(&node->policy_state, now_ms);
#endif
#if UCN_TEST_NODE_STEP_STAGE_LIMIT == 3
    return UCN_ERR_NOT_FOUND;
#endif
#if UCN_FEATURE_PATH
    ucn_path_expire(&node->path_state, now_ms);
#endif
#if UCN_FEATURE_DIAGNOSTICS
    expire_path_trace_state(node, now_ms);
    expire_node_snapshot_state(node, now_ms);
    expire_policy_diagnostic_state(node, now_ms);
#endif
    expire_neighbor_candidates(node, now_ms);
#if UCN_TEST_NODE_STEP_STAGE_LIMIT == 4
    return UCN_ERR_NOT_FOUND;
#endif
    maintain_neighbor_liveness(node, now_ms);
#if UCN_TEST_NODE_STEP_STAGE_LIMIT == 5
    return UCN_ERR_NOT_FOUND;
#endif
    evaluate_bearer_quality(node, now_ms);
#if UCN_TEST_NODE_STEP_STAGE_LIMIT == 6
    return UCN_ERR_NOT_FOUND;
#endif

    item = select_business_item(node, &next_schedule_cursor);

    /* A permanently non-empty business queue must not indefinitely suppress
     * neighbor liveness or path maintenance.  The burst counter saturates,
     * therefore once a control action becomes due it gets a scheduling slot
     * in this call.  Q0 remains FIFO and is delayed only by an actually-due
     * essential maintenance action; diagnostics never preempt a business
     * queue.  Q0-Q3 arbitration is handled by the bounded weighted schedule. */
    if (node->business_tx_since_maintenance >=
        UCN_BUSINESS_TX_BURST_BEFORE_MAINTENANCE) {
        result = send_due_essential_maintenance(node, now_ms);
        if (result != UCN_ERR_NOT_FOUND) {
            node->business_tx_since_maintenance = 0U;
            node->stats.maintenance_preemptions++;
            return result;
        }
    }

    if (item == NULL) {
        result = send_pending_q1_if_ready(node, now_ms);
        if (result != UCN_ERR_NOT_FOUND) {
            note_business_transmission(node);
            return result;
        }
        result = send_due_essential_maintenance(node, now_ms);
        if (result != UCN_ERR_NOT_FOUND) {
            node->business_tx_since_maintenance = 0U;
            return result;
        }
#if UCN_FEATURE_DIAGNOSTICS
        result = send_due_node_snapshot_reply(node, now_ms);
        if (result != UCN_ERR_NOT_FOUND) {
            return result;
        }
        /* T22.6 queries are deliberately after normal Q0-Q3 work, liveness,
         * route maintenance and existing diagnostics.  They never enter the
         * business queues and cannot consume a Q0 slot. */
        result = send_due_policy_diagnostic_reply(node);
        if (result != UCN_ERR_NOT_FOUND) {
            return result;
        }
        result = send_due_policy_diagnostic_request(node);
        if (result != UCN_ERR_NOT_FOUND) {
            return result;
        }
#endif
#if UCN_FEATURE_CANDIDATE_ROUTING
        return start_due_route_refresh(node, now_ms);
#else
        return UCN_ERR_NOT_FOUND;
#endif
    }

    node->stats.tx_scheduled_by_class[(uint8_t)item->traffic_class]++;

    if (ucn_deadline_expired(now_ms, item->deadline_ms)) {
#if UCN_FEATURE_DYNAMIC_MESH
        if (item->waiting_for_route) {
            node->stats.q0_route_wait_expired++;
        } else
#endif
        if (item->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE &&
            item->backpressure_retries != 0U) {
            node->stats.q0_backpressure_expired++;
        }
        item->occupied = false;
        node->business_schedule_cursor = next_schedule_cursor;
        node->stats.tx_expired_dropped++;
        return UCN_ERR_TTL;
    }

    /* A retained Q0 item preserves FIFO ownership while it waits for either
     * local backpressure or a dynamic Route to clear.  Lower-priority business
     * and diagnostics do not use the gap, but essential liveness/path
     * maintenance may proceed and drive the pending discovery. */
    if (item->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE &&
        item->next_attempt_ms != 0U &&
        !ucn_deadline_expired(now_ms, item->next_attempt_ms)) {
        result = send_due_essential_maintenance(node, now_ms);
        if (result != UCN_ERR_NOT_FOUND) {
            node->business_tx_since_maintenance = 0U;
        }
        return result;
    }

    count = item->payload_length;
    error_drops_before = node->stats.tx_error_dropped;
#if UCN_FEATURE_DYNAMIC_MESH
    was_waiting_for_route = item->waiting_for_route;
#endif
    result = ucn_node_send(node,
                           item->destination,
                           item->message_type,
                           item->traffic_class,
                           item->payload,
                           (uint16_t)count);
    note_business_transmission(node);
    if (result == UCN_OK) {
#if UCN_FEATURE_DYNAMIC_MESH
        if (was_waiting_for_route) {
            node->stats.q0_route_wait_recovered++;
        }
#endif
        node->stats.tx_queue_sent_by_class[(uint8_t)item->traffic_class]++;
        item->occupied = false;
        node->business_schedule_cursor = next_schedule_cursor;
        return UCN_OK;
    }

#if UCN_FEATURE_DYNAMIC_MESH
    if (result == UCN_ERR_NOT_FOUND &&
        item->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE) {
        const ucn_result_t discovery_result = ensure_q0_route_discovery(
            node, item->destination, now_ms);

        if (discovery_result == UCN_OK ||
            discovery_result == UCN_ERR_NO_SPACE ||
            discovery_result == UCN_ERR_NOT_FOUND) {
            node->stats.tx_error_dropped = error_drops_before;
            if (was_waiting_for_route) {
                node->stats.q0_route_wait_retried++;
            } else {
                node->stats.q0_route_wait_started++;
            }
            item->waiting_for_route = true;
            item->next_attempt_ms = ucn_deadline_from_now(
                now_ms, UCN_Q0_ROUTE_WAIT_RETRY_INTERVAL_MS);
            return result;
        }
        route_wait_failure = true;
    }
    if (was_waiting_for_route && result == UCN_ERR_NO_SPACE) {
        item->waiting_for_route = false;
        node->stats.q0_route_wait_recovered++;
    }
#endif

    if (result == UCN_ERR_NO_SPACE &&
        item->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE) {
        const bool retry_budget_available =
            item->backpressure_retries < UCN_Q0_BACKPRESSURE_MAX_RETRIES;
        const bool retry_fits_deadline =
            !ucn_deadline_due_within(
                now_ms, item->deadline_ms,
                UCN_Q0_BACKPRESSURE_RETRY_INTERVAL_MS);

        if (retry_budget_available && retry_fits_deadline) {
            /* send_frame_on_link() records a failed attempt as a drop.  The
             * item is still owned here, so restore the pre-attempt final-drop
             * count.  A later final failure will be counted exactly once. */
            node->stats.tx_error_dropped = error_drops_before;
            item->backpressure_retries++;
            item->next_attempt_ms = ucn_deadline_from_now(
                now_ms, UCN_Q0_BACKPRESSURE_RETRY_INTERVAL_MS);
            node->stats.q0_backpressure_retries++;
            return result;
        }
        item->occupied = false;
        node->business_schedule_cursor = next_schedule_cursor;
        node->stats.q0_backpressure_exhausted++;
        if (node->stats.tx_error_dropped == error_drops_before) {
            node->stats.tx_error_dropped++;
        }
        return result;
    }

    item->occupied = false;
    node->business_schedule_cursor = next_schedule_cursor;
    if (node->stats.tx_error_dropped == error_drops_before) {
        node->stats.tx_error_dropped++;
    }
    if (item->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE) {
#if UCN_FEATURE_DYNAMIC_MESH
        if (was_waiting_for_route || route_wait_failure) {
            node->stats.q0_route_wait_terminal_failed++;
        } else
#endif
        node->stats.q0_backpressure_terminal_failed++;
    }
    return result;
}

/*
 * EN: Returns the current `stats` view from Lite/Full Node state.
 * 中文：从 Lite/Full Node 状态返回当前 `stats` 视图。
 */
const ucn_node_stats_t *ucn_node_get_stats(const ucn_node_t *node)
{
    return node == NULL ? NULL : &node->stats;
}

/*
 * EN: Validates one received frame and routes, forwards, or locally delivers it.
 * 中文：验证一个接收帧，并对其进行路由、转发或本地投递。
 */
ucn_result_t ucn_node_receive(ucn_node_t *node,
                              ucn_link_t *ingress_link,
                              const uint8_t *data,
                              size_t length)
{
    ucn_frame_t frame;
    bool control_consumed = false;
    ucn_wire_profile_t local_receive_profile;
    ucn_wire_profile_t incoming_profile;
    ucn_result_t result;
    uint8_t plaintext[UCN_MAX_PAYLOAD_BYTES];

    if (node == NULL || ingress_link == NULL || data == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_node_security_ready(node)) {
        return UCN_ERR_SECURITY;
    }

    result = resolve_link_local_receive_profile(node, ingress_link,
                                                &local_receive_profile);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_frame_peek_wire_profile(data, length, &incoming_profile);
    if (result != UCN_OK) {
        return result;
    }
    if (incoming_profile > local_receive_profile) {
        return UCN_ERR_UNSUPPORTED;
    }
    result = ucn_frame_decode(data, length, &frame);
    if (result != UCN_OK) {
        return result;
    }

    if (frame.network_id != node->config.network_id) {
        return UCN_ERR_NETWORK;
    }
    result = validate_inbound_hop_scope(node, &frame);
    if (result != UCN_OK) {
        return result;
    }

    if (ucn_message_type_is_control(frame.message_type) &&
        (frame.flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U) {
        return UCN_ERR_MALFORMED;
    }
    if (ucn_message_type_is_control(frame.message_type) && frame.has_path_id) {
        return UCN_ERR_MALFORMED;
    }
#if !UCN_FEATURE_PATH
    if (frame.has_path_id || frame.message_type == UCN_MSG_PATH_INSTALL ||
        frame.message_type == UCN_MSG_PATH_REVOKE) {
        return UCN_ERR_CONFIG;
    }
#endif
#if !UCN_FEATURE_CANDIDATE_ROUTING
    if (frame.message_type == UCN_MSG_PATH_PROBE ||
        frame.message_type == UCN_MSG_PATH_PROBE_ACK ||
        frame.message_type == UCN_MSG_PATH_ACTIVATE ||
        frame.message_type == UCN_MSG_PATH_ACTIVATE_ACK) {
        return UCN_ERR_CONFIG;
    }
#endif
#if !UCN_FEATURE_DIAGNOSTICS
    if (frame.message_type == UCN_MSG_PATH_TRACE_REQ ||
        frame.message_type == UCN_MSG_PATH_TRACE_REPLY ||
        frame.message_type == UCN_MSG_NODE_SNAPSHOT_REQ ||
        frame.message_type == UCN_MSG_NODE_SNAPSHOT_REPLY ||
        frame.message_type == UCN_MSG_POLICY_DIAGNOSTIC_REQ ||
        frame.message_type == UCN_MSG_POLICY_DIAGNOSTIC_REPLY ||
        (frame.flags & UCN_FRAME_FLAG_DIAGNOSTIC) != 0U) {
        return UCN_ERR_CONFIG;
    }
#endif

    if (node->security_ops != NULL) {
        result = node->security_ops->authorize_rx(node->security_context,
                                                  ingress_link, &frame);
        if (result != UCN_OK) {
            if (frame.destination == node->config.node_id) {
#if UCN_FEATURE_PATH
                note_path_control_authorization_rejected(node,
                                                         frame.message_type);
#endif
            }
            return result;
        }
    }

    if (frame.message_type == UCN_MSG_HELLO) {
        result = ucn_duplicate_accept_frame(node, &frame);
        if (result != UCN_OK) {
            return result;
        }
        return handle_hello(node, ingress_link, &frame);
    }

    /* A Link becomes eligible for regular mesh traffic only after it was
     * configured statically or admitted through HELLO plus the join policy. */
    if (!link_is_registered(node, ingress_link)) {
        return UCN_ERR_ACCESS;
    }

    result = validate_inbound_business_security(node, ingress_link, &frame, plaintext);
    if (result != UCN_OK) {
        return result;
    }

    if (frame.message_type == UCN_MSG_ROUTE_REQ) {
        ucn_rreq_cache_classification_t classification;
        size_t rreq_slot = 0U;
        ucn_route_cost_t route_cost;

        if (frame.destination != UCN_NODE_BROADCAST) {
            return UCN_ERR_MALFORMED;
        }
        if (frame.payload_length != route_request_payload_size(
                                       frame.wire_profile)) {
            return UCN_ERR_MALFORMED;
        }
        result = validate_route_request_frame(node, ingress_link, &frame);
        if (result != UCN_OK) {
            return result;
        }
        route_cost = read_route_cost_for_profile(
            frame.payload + route_request_cost_offset(&frame),
            frame.wire_profile);
        classification = classify_route_request(node, &frame, route_cost,
                                                &rreq_slot);
        if (classification == UCN_RREQ_CACHE_REPLAY) {
            node->stats.route_request_replayed++;
            return UCN_ERR_REPLAY;
        }
        if (classification == UCN_RREQ_CACHE_FULL) {
            node->stats.route_request_cache_full++;
            return UCN_ERR_NO_SPACE;
        }
        if (!take_control_rx_token(node, ingress_link,
                                   UCN_CONTROL_RX_ROUTE_REQUEST)) {
            return UCN_ERR_NO_SPACE;
        }
        commit_route_request(node, &frame, route_cost, rreq_slot);
        touch_neighbor(node, ingress_link);
        return handle_route_request(node, ingress_link, &frame);
    }

    result = ucn_duplicate_accept_frame(node, &frame);
    if (result != UCN_OK) {
        return result;
    }

    if (frame.message_type == UCN_MSG_HEARTBEAT) {
        return handle_heartbeat(node, ingress_link, &frame);
    }

    touch_neighbor(node, ingress_link);

#if UCN_FEATURE_DIAGNOSTICS
    if (frame.message_type == UCN_MSG_NODE_SNAPSHOT_REQ) {
        return handle_node_snapshot_request(node, ingress_link, &frame);
    }
    if (frame.message_type == UCN_MSG_NODE_SNAPSHOT_REPLY) {
        return handle_node_snapshot_reply(node, ingress_link, &frame);
    }
    if (frame.message_type == UCN_MSG_POLICY_DIAGNOSTIC_REQ &&
        frame.destination == node->config.node_id) {
        return handle_policy_diagnostic_request(node, &frame);
    }
    if (frame.message_type == UCN_MSG_POLICY_DIAGNOSTIC_REPLY &&
        frame.destination == node->config.node_id) {
        return handle_policy_diagnostic_reply(node, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_TRACE_REQ) {
        return handle_path_trace_request(node, ingress_link, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_TRACE_REPLY) {
        return handle_path_trace_reply(node, ingress_link, &frame);
    }
#endif
#if UCN_FEATURE_PATH
    if (frame.message_type == UCN_MSG_PATH_INSTALL &&
        frame.destination == node->config.node_id) {
        return handle_path_install(node, ingress_link, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_REVOKE &&
        frame.destination == node->config.node_id) {
        return handle_path_revoke(node, ingress_link, &frame);
    }
#endif
#if UCN_FEATURE_CANDIDATE_ROUTING
    if (frame.message_type == UCN_MSG_PATH_PROBE &&
        frame.destination == node->config.node_id) {
        return handle_path_probe(node, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_PROBE_ACK &&
        frame.destination == node->config.node_id) {
        return handle_path_probe_ack(node, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_ACTIVATE) {
        return handle_path_activate(node, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_ACTIVATE_ACK &&
        frame.destination == node->config.node_id) {
        return handle_path_activate_ack(node, &frame);
    }
#endif

    if (frame.message_type == UCN_MSG_ROUTE_REPLY) {
        result = handle_route_reply(node, ingress_link, &frame, &control_consumed);
        if (result != UCN_OK) {
            return result;
        }
        if (control_consumed) {
            return UCN_OK;
        }
    }

    if (frame.message_type == UCN_MSG_ROUTE_ERROR) {
        result = handle_route_error(node, &frame, &control_consumed);
        if (result != UCN_OK) {
            return result;
        }
        if (control_consumed) {
            return UCN_OK;
        }
    }

#if UCN_FEATURE_PATH
    if (!ucn_message_type_is_control(frame.message_type) && frame.has_path_id) {
        const ucn_path_forward_entry_t *path = find_active_path(
            node, frame.source, frame.session_id, frame.path_id, frame.destination);

        if (path == NULL ||
            (frame.destination == node->config.node_id && !path->terminal) ||
            (frame.destination != node->config.node_id && path->terminal) ||
            (path != NULL &&
             frame.hop_limit != (uint8_t)(path->remaining_hops + 1U))) {
            node->stats.path_rejected++;
            (void)send_path_route_error(node, ingress_link, frame.source,
                                        frame.destination, frame.session_id,
                                        frame.path_id, frame.wire_profile);
            return UCN_ERR_NOT_FOUND;
        }
    }
#endif

    if (!ucn_message_type_is_control(frame.message_type) && !frame.has_path_id &&
        frame.has_route_extension &&
        frame.destination == node->config.node_id &&
        !route_epoch_is_accepted(node, frame.source, &frame)) {
        node->stats.route_epoch_rejected++;
        return UCN_ERR_NOT_FOUND;
    }

    if (frame.destination != node->config.node_id &&
        frame.destination != UCN_NODE_BROADCAST) {
        ucn_link_t *egress_link;
#if UCN_FEATURE_PATH
        const ucn_path_forward_entry_t *path = NULL;
#endif
        uint8_t route_reply_payload[UCN_ROUTE_REPLY_MAX_PAYLOAD_BYTES];

        if (frame.hop_limit <= 1U) {
            return UCN_ERR_TTL;
        }

#if UCN_FEATURE_CANDIDATE_ROUTING
        if (frame.message_type == UCN_MSG_ROUTE_REPLY &&
            frame.payload_length == route_reply_payload_size(
                                        frame.wire_profile) &&
            (frame.payload[route_reply_flags_offset(&frame)] &
             UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U) {
            egress_link = find_candidate_link(node, frame.destination,
                                              read_u32_be(frame.payload));
        } else if (frame.message_type == UCN_MSG_PATH_PROBE ||
                   frame.message_type == UCN_MSG_PATH_PROBE_ACK) {
            if (frame.payload_length != UCN_PATH_PROBE_PAYLOAD_BYTES ||
                read_u32_be(frame.payload) == 0U) {
                return UCN_ERR_MALFORMED;
            }
            egress_link = find_candidate_link(node, frame.destination,
                                              read_u32_be(frame.payload));
#if UCN_FEATURE_PATH
        } else if (!ucn_message_type_is_control(frame.message_type) &&
                   frame.has_path_id) {
            path = find_active_path(
                node, frame.source, frame.session_id, frame.path_id,
                frame.destination);

            egress_link = path == NULL ? NULL :
                          resolve_egress_link(node, path->egress_link);
#endif
        } else if (!ucn_message_type_is_control(frame.message_type)) {
#else
#if UCN_FEATURE_PATH
        if (!ucn_message_type_is_control(frame.message_type) &&
            frame.has_path_id) {
            path = find_active_path(
                node, frame.source, frame.session_id, frame.path_id,
                frame.destination);
            egress_link = path == NULL ? NULL :
                          resolve_egress_link(node, path->egress_link);
        } else
#endif
        if (!ucn_message_type_is_control(frame.message_type)) {
#endif
            egress_link = find_link_for_route_epoch(node, frame.destination,
                                                     frame.has_route_extension,
                                                     frame.route_epoch);
        } else {
            egress_link = find_link(node, frame.destination);
        }
        if (egress_link == NULL || egress_link == ingress_link) {
#if UCN_FEATURE_PATH
            if (frame.has_path_id) {
                if (egress_link == NULL && path != NULL) {
                    revoke_path_and_mark_local_policy(node, path->owner,
                                                       path->owner_session_id,
                                                       path->path_id,
                                                       path->destination);
                }
                (void)send_path_route_error(node, ingress_link, frame.source,
                                            frame.destination, frame.session_id,
                                            frame.path_id, frame.wire_profile);
            } else if (frame.message_type != UCN_MSG_ROUTE_ERROR) {
                (void)send_route_error(node, ingress_link, frame.source,
                                       frame.destination, frame.wire_profile);
            }
#else
            if (frame.message_type != UCN_MSG_ROUTE_ERROR) {
                (void)send_route_error(node, ingress_link, frame.source,
                                       frame.destination, frame.wire_profile);
            }
#endif
            return UCN_ERR_NOT_FOUND;
        }

        /* handle_route_reply() learned this node's target-rooted metric from
         * the ingress Link.  Forward the same accumulated Cost/Hop so the
         * upstream node can extend it by exactly one more Link. */
        if (frame.message_type == UCN_MSG_ROUTE_REPLY &&
            frame.payload_length == route_reply_payload_size(
                                        frame.wire_profile)) {
            ucn_route_cost_t forwarded_cost;
            uint8_t reply_hop_count =
                frame.payload[route_reply_hop_offset(&frame)];

            if (reply_hop_count == UINT8_MAX) {
                return UCN_ERR_TTL;
            }
            (void)memcpy(route_reply_payload, frame.payload,
                         frame.payload_length);
            result = accumulate_route_cost(
                read_route_cost_for_profile(
                    frame.payload + route_reply_cost_offset(),
                    frame.wire_profile),
                link_route_cost(ingress_link), &forwarded_cost);
            if (result != UCN_OK) {
                return result;
            }
            result = write_route_cost_for_profile(
                route_reply_payload + route_reply_cost_offset(),
                frame.wire_profile, forwarded_cost);
            if (result != UCN_OK) {
                return result;
            }
            route_reply_payload[route_reply_hop_offset(&frame)] =
                (uint8_t)(reply_hop_count + 1U);
            frame.payload = route_reply_payload;
        }

        --frame.hop_limit;
#if UCN_FEATURE_PATH
        result = frame.has_path_id ?
                 send_frame_on_path_egress(node, path, &frame, &egress_link) :
                 send_frame_on_logical_egress(node, egress_link, &frame,
                                              &egress_link);
#else
        result = send_frame_on_logical_egress(node, egress_link, &frame,
                                              &egress_link);
#endif
        if (result == UCN_OK) {
            mark_route_used(node, frame.destination);
#if UCN_FEATURE_PATH
            if (frame.has_path_id) {
                node->stats.path_forwards++;
            }
#endif
            if ((frame.flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U) {
                node->stats.e2e_protected_forwarded++;
            }
        }
        if (result == UCN_ERR_LINK_DOWN) {
            invalidate_routes_by_link(node, egress_link);
        }
#if UCN_FEATURE_PATH
        if (frame.has_path_id &&
            (result == UCN_ERR_LINK_DOWN ||
             path_result_is_capability_failure(result))) {
            if (path_result_is_capability_failure(result)) {
                node->stats.path_capability_failures++;
            }
            revoke_path_and_mark_local_policy(node, frame.source,
                                               frame.session_id, frame.path_id,
                                               frame.destination);
            (void)send_path_route_error(node, ingress_link, frame.source,
                                        frame.destination, frame.session_id,
                                        frame.path_id, frame.wire_profile);
        } else if (result == UCN_ERR_LINK_DOWN &&
                   frame.message_type != UCN_MSG_ROUTE_ERROR) {
            (void)send_route_error(node, ingress_link, frame.source,
                                   frame.destination, frame.wire_profile);
        }
#else
        if (result == UCN_ERR_LINK_DOWN) {
            if (frame.message_type != UCN_MSG_ROUTE_ERROR) {
                (void)send_route_error(node, ingress_link, frame.source,
                                       frame.destination, frame.wire_profile);
            }
        }
#endif
        return result;
    }

    if (!dispatch_endpoint(node, &frame) && node->rx_handler != NULL) {
        node->rx_handler(node->rx_context, &frame);
    }

    node->stats.rx_delivered++;

    return UCN_OK;
}
