#include <stddef.h>
#include <string.h>

#include "ucn/ucn_adapter.h"
#include "ucn/ucn_cluster.h"
#include "ucn/ucn_cluster_config_state.h"
#include "ucn/ucn_cluster_config_tx.h"
#include "ucn/ucn_cluster_config_proposal.h"
#include "ucn/ucn_cluster_config_quorum.h"
#include "ucn/ucn_cluster_config_store.h"
#include "ucn/ucn_cluster_config_persistence.h"
#include "ucn/ucn_cluster_config_backup.h"
#include "ucn/ucn_cluster_config_joint.h"
#include "ucn/ucn_cluster_authority.h"
#include "ucn/ucn_cluster_membership.h"
#include "ucn/ucn_cluster_wire_v4.h"
#include "ucn/ucn_cluster_federation.h"
#include "ucn/ucn_cluster_persist.h"
#include "ucn/ucn_node.h"
#include "ucn/ucn_path.h"
#include "ucn/ucn_standard_adapter.h"
#include "ucn/ucn_transfer.h"
#include "ucn/ports/ucn_port_bare_metal.h"
#include "ucn/ports/ucn_port_freertos.h"
#include "ucn/ports/ucn_port_host_fake.h"
#include "ucn/ports/ucn_port_nuttx.h"
#include "ucn/ports/ucn_port_rtthread.h"
#include "ucn/ports/ucn_port_zephyr.h"
#include "ucn/ports/ucn_event_runtime.h"
#include "ucn/adapters/ucn_can_source.h"
#include "ucn/adapters/ucn_stream_source.h"
#if UCN_FEATURE_SERVICE
#include "ucn/ucn_service_bridge.h"
#endif

/* This translation unit deliberately does not include ucn_node_storage.h.
 * It proves that pointer-only application/Adapter/Bridge declarations can use
 * the public API without seeing the Node implementation layout. */
static uint32_t public_header_now_ms(void *context)
{
    (void)context;
    return 0U;
}

static ucn_result_t public_persist_load(
    void *context,
    ucn_cluster_persist_load_result_t *result)
{
    (void)context;
    if (result == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(result, 0, sizeof(*result));
    result->state = UCN_CLUSTER_PERSIST_LOAD_FACTORY_EMPTY;
    return UCN_OK;
}

static ucn_cluster_persist_completion_t public_persist_submit(
    void *context,
    const ucn_cluster_persist_request_t *request)
{
    ucn_cluster_persist_completion_t completion;

    (void)context;
    completion.state = request == NULL ? UCN_CLUSTER_PERSIST_FAILED :
                                        UCN_CLUSTER_PERSIST_COMMITTED;
    completion.token = UCN_CLUSTER_PERSIST_TOKEN_NONE;
    completion.failure = request == NULL ? UCN_ERR_ARGUMENT : UCN_OK;
    return completion;
}

static ucn_cluster_persist_completion_t public_persist_poll(
    void *context,
    ucn_cluster_persist_token_t token)
{
    ucn_cluster_persist_completion_t completion;

    (void)context;
    completion.state = token == UCN_CLUSTER_PERSIST_TOKEN_NONE ?
                           UCN_CLUSTER_PERSIST_FAILED :
                           UCN_CLUSTER_PERSIST_COMMITTED;
    completion.token = UCN_CLUSTER_PERSIST_TOKEN_NONE;
    completion.failure = token == UCN_CLUSTER_PERSIST_TOKEN_NONE ?
                             UCN_ERR_ARGUMENT : UCN_OK;
    return completion;
}

int test_public_headers(void)
{
    ucn_node_t *node = NULL;
    const ucn_path_capability_t capability = {
        UCN_WIRE_PROFILE_W0_LOCAL,
        64U
    };
    ucn_link_t legacy_link;
    const ucn_path_forward_config_t legacy_path = {
        UINT32_C(1), UINT32_C(2), UINT32_C(3), UINT32_C(4), UINT32_C(5),
        2U, &legacy_link, UINT32_C(1000)
    };
    const ucn_port_ops_t port_ops_v2 = {
        .struct_size = (uint16_t)sizeof(ucn_port_ops_t),
        .api_version = UCN_PORT_OPS_API_VERSION,
        .now_ms = public_header_now_ms
    };
    const ucn_cluster_persist_provider_t persist_provider_v1 = {
        .struct_size = (uint16_t)sizeof(ucn_cluster_persist_provider_t),
        .api_version = UCN_CLUSTER_PERSIST_PROVIDER_API_VERSION,
        .load = public_persist_load,
        .submit = public_persist_submit,
        .poll = public_persist_poll
    };
    ucn_cluster_persist_request_t persist_request = {
        .operation_id = 1U,
        .operation = UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT,
        .next_state = {
            .has_active_epoch = true,
            .active_epoch = { 1U, 2U, 3U },
            .has_max_epoch = true,
            .max_epoch = { 1U, 2U, 3U },
            .config_transaction = {
                .phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE
            },
            .rekey_transaction = {
                .phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE
            }
        }
    };
    const ucn_transfer_config_t transfer_config_v2 = {
        .node = node,
        .now_ms = public_header_now_ms,
        .max_retries = 3U,
        .ack_timeout_ms = UINT32_C(100),
        .rx_timeout_ms = UINT32_C(200),
        .completed_hold_ms = UINT32_C(300),
        .recent_completion_ms = UINT32_C(400)
    };
    ucn_result_t (*step_fn)(ucn_node_t *, uint32_t) = ucn_node_step;
    const ucn_node_stats_t *(*stats_fn)(const ucn_node_t *) =
        ucn_node_get_stats;
    ucn_result_t (*preset_fn)(ucn_standard_preset_t,
                              ucn_standard_preset_profile_t *) =
        ucn_standard_preset_resolve;
    ucn_result_t (*isr_enqueue_fn)(ucn_adapter_rx_queue_t *, ucn_link_t *,
                                   const uint8_t *, size_t) =
        ucn_adapter_rx_enqueue_from_isr;
    ucn_result_t (*path_install_capable_fn)(
        ucn_path_state_t *, const ucn_path_forward_config_t *,
        const ucn_path_capability_t *) = ucn_path_install_capable;
    bool (*member_record_valid_fn)(const ucn_cluster_member_t *) =
        ucn_cluster_member_record_is_valid;
    bool (*member_table_valid_fn)(const ucn_cluster_member_table_t *) =
        ucn_cluster_member_table_is_valid;
    size_t (*member_table_count_fn)(const ucn_cluster_member_table_t *) =
        ucn_cluster_member_table_count;
    bool (*voter_set_valid_fn)(const ucn_cluster_voter_set_t *) =
        ucn_cluster_voter_set_is_valid;
    bool (*voter_set_build_fn)(ucn_cluster_voter_set_t *, uint32_t,
                               const ucn_node_id_t *, size_t) =
        ucn_cluster_voter_set_build;
    bool (*voter_set_contains_fn)(const ucn_cluster_voter_set_t *,
                                  ucn_node_id_t) = ucn_cluster_voter_set_contains;
    uint8_t (*voter_set_quorum_fn)(const ucn_cluster_voter_set_t *) =
        ucn_cluster_voter_set_quorum;
    bool (*voter_set_bitmap_fn)(const ucn_cluster_voter_set_t *,
                                ucn_node_id_t, uint64_t *) =
        ucn_cluster_voter_set_bitmap_for_node;
    ucn_result_t (*bare_metal_init_fn)(
        ucn_bare_metal_port_t *, const ucn_protocol_owner_config_t *) =
        ucn_bare_metal_port_init;
    ucn_result_t (*freertos_init_fn)(
        ucn_freertos_port_t *, const ucn_freertos_port_config_t *) =
        ucn_freertos_port_init;
    ucn_result_t (*zephyr_init_fn)(
        ucn_zephyr_port_t *, const ucn_zephyr_port_config_t *) =
        ucn_zephyr_port_init;
    ucn_result_t (*nuttx_init_fn)(
        ucn_nuttx_port_t *, const ucn_nuttx_port_config_t *) =
        ucn_nuttx_port_init;
    ucn_result_t (*rtthread_init_fn)(
        ucn_rtthread_port_t *, const ucn_rtthread_port_config_t *) =
        ucn_rtthread_port_init;
    ucn_result_t (*host_fake_init_fn)(
        ucn_host_fake_port_t *, const ucn_host_fake_port_config_t *) =
        ucn_host_fake_port_init;
    ucn_result_t (*event_runtime_init_fn)(
        ucn_event_runtime_t *, const ucn_event_runtime_config_t *) =
        ucn_event_runtime_init;
    ucn_result_t (*stream_source_init_fn)(
        ucn_stream_source_t *, const ucn_stream_source_config_t *) =
        ucn_stream_source_init;
    ucn_result_t (*stream_source_write_isr_fn)(
        ucn_stream_source_t *, const uint8_t *, size_t) =
        ucn_stream_source_write_from_isr;
    ucn_result_t (*stream_carrier_encode_fn)(
        const uint8_t *, size_t, uint8_t *, size_t, size_t *) =
        ucn_stream_carrier_encode;
    ucn_result_t (*can_source_init_fn)(
        ucn_can_source_t *, const ucn_can_source_config_t *) =
        ucn_can_source_init;
    ucn_result_t (*can_source_write_isr_fn)(
        ucn_can_source_t *, const ucn_can_frame_t *) =
        ucn_can_source_write_from_isr;
    ucn_result_t (*can_fd_encode_fn)(
        const uint8_t *, size_t, uint8_t *, size_t *) =
        ucn_can_fd_carrier_encode;
    size_t (*transfer_class_size_fn)(ucn_transfer_class_t) =
        ucn_transfer_class_max_bytes;
    ucn_result_t (*transfer_peer_window_fn)(ucn_transfer_t *, ucn_node_id_t,
                                             uint8_t) =
        ucn_transfer_set_peer_window_capability;
    ucn_result_t (*transfer_peer_concurrency_fn)(
        ucn_transfer_t *, ucn_node_id_t, uint8_t) =
        ucn_transfer_set_peer_concurrency_capability;
    ucn_result_t (*transfer_local_window_fn)(ucn_transfer_t *, uint8_t) =
        ucn_transfer_set_tx_window_size;
    ucn_result_t (*transfer_step_fn)(ucn_transfer_t *) = ucn_transfer_step;
    size_t (*neighbor_summary_fn)(const ucn_node_t *,
                                  ucn_neighbor_summary_t *, size_t) =
        ucn_node_copy_neighbor_summaries;
    ucn_result_t (*cluster_init_fn)(ucn_cluster_t *,
                                    const ucn_cluster_config_t *) =
        ucn_cluster_init;
    ucn_result_t (*cluster_step_fn)(ucn_cluster_t *) = ucn_cluster_step;
    ucn_result_t (*cluster_score_fn)(ucn_cluster_t *, uint16_t) =
        ucn_cluster_set_head_score;
    ucn_result_t (*cluster_view_fn)(const ucn_cluster_t *, ucn_cluster_view_t *) =
        ucn_cluster_get_view;
    size_t (*cluster_members_fn)(const ucn_cluster_t *,
                                 ucn_cluster_member_summary_t *, size_t) =
        ucn_cluster_copy_member_summaries;
    ucn_result_t (*cluster_member_at_fn)(const ucn_cluster_t *, size_t,
                                         ucn_cluster_member_summary_t *) =
        ucn_cluster_get_member_summary_at;
    ucn_result_t (*cluster_member_capacity_fn)(
        const ucn_cluster_t *, ucn_cluster_member_capacity_view_t *) =
        ucn_cluster_get_member_capacity_view;
    size_t (*federation_size_fn)(const ucn_cluster_federation_message_t *) =
        ucn_cluster_federation_message_encoded_size;
    ucn_result_t (*federation_decode_fn)(
        const uint8_t *, size_t, ucn_cluster_federation_message_t *) =
        ucn_cluster_federation_message_decode;
    ucn_result_t (*federation_init_fn)(
        ucn_cluster_federation_t *, const ucn_cluster_federation_config_t *) =
        ucn_cluster_federation_init;
    ucn_result_t (*federation_receive_fn)(ucn_cluster_federation_t *,
                                           ucn_node_id_t, bool,
                                           const uint8_t *, size_t) =
        ucn_cluster_federation_receive;
    ucn_result_t (*federation_step_fn)(ucn_cluster_federation_t *) =
        ucn_cluster_federation_step;
    ucn_result_t (*federation_query_fn)(ucn_cluster_federation_t *,
                                         ucn_node_id_t) =
        ucn_cluster_federation_query_locator;
    ucn_result_t (*federation_send_fn)(
        ucn_cluster_federation_t *, ucn_node_id_t, ucn_endpoint_t,
        ucn_traffic_class_t, const uint8_t *, uint16_t) =
        ucn_cluster_federation_send;
    const ucn_cluster_locator_t *(*federation_find_fn)(
        const ucn_cluster_federation_t *, ucn_node_id_t) =
        ucn_cluster_federation_find_locator;
    const ucn_cluster_federation_next_cluster_entry_t *(*federation_next_fn)(
        const ucn_cluster_federation_t *, uint32_t) =
        ucn_cluster_federation_find_next_cluster;
    const ucn_cluster_federation_stats_t *(*federation_stats_fn)(
        const ucn_cluster_federation_t *) = ucn_cluster_federation_get_stats;
    ucn_cluster_persist_completion_t persist_completion;

    if (ucn_cluster_persist_request_finalize(&persist_request) != UCN_OK) {
        return 1;
    }
    persist_completion = persist_provider_v1.submit(
        persist_provider_v1.context, &persist_request);

    return node == NULL && step_fn != NULL && stats_fn != NULL && preset_fn != NULL &&
           isr_enqueue_fn != NULL && path_install_capable_fn != NULL &&
           member_record_valid_fn != NULL && member_table_valid_fn != NULL &&
           member_table_count_fn != NULL &&
           voter_set_valid_fn != NULL && voter_set_build_fn != NULL &&
           voter_set_contains_fn != NULL && voter_set_quorum_fn != NULL &&
           voter_set_bitmap_fn != NULL &&
           bare_metal_init_fn != NULL && freertos_init_fn != NULL &&
           zephyr_init_fn != NULL && nuttx_init_fn != NULL &&
           rtthread_init_fn != NULL && host_fake_init_fn != NULL &&
           event_runtime_init_fn != NULL &&
           stream_source_init_fn != NULL &&
           stream_source_write_isr_fn != NULL &&
           stream_carrier_encode_fn != NULL &&
           can_source_init_fn != NULL &&
           can_source_write_isr_fn != NULL &&
           can_fd_encode_fn != NULL &&
           transfer_peer_window_fn != NULL &&
           transfer_peer_concurrency_fn != NULL &&
            transfer_local_window_fn != NULL && transfer_step_fn != NULL &&
            neighbor_summary_fn != NULL && cluster_init_fn != NULL &&
            cluster_step_fn != NULL && cluster_score_fn != NULL &&
             cluster_view_fn != NULL && cluster_members_fn != NULL &&
             cluster_member_at_fn != NULL && cluster_member_capacity_fn != NULL &&
             federation_size_fn != NULL &&
             federation_decode_fn != NULL && federation_init_fn != NULL &&
             federation_receive_fn != NULL && federation_step_fn != NULL &&
             federation_query_fn != NULL && federation_send_fn != NULL &&
            federation_find_fn != NULL &&
            federation_next_fn != NULL && federation_stats_fn != NULL &&
            ucn_port_ops_is_compatible(&port_ops_v2) &&
            ucn_cluster_persist_provider_is_compatible(&persist_provider_v1) &&
            ucn_cluster_persist_provider_supports_async(&persist_provider_v1) &&
            ucn_cluster_persist_completion_is_valid(&persist_completion) &&
            ucn_cluster_persist_request_is_valid(&persist_request) &&
            public_persist_load(NULL, NULL) == UCN_ERR_ARGUMENT &&
            ucn_cluster_persist_completion_is_valid(
                &(ucn_cluster_persist_completion_t){
                    UCN_CLUSTER_PERSIST_PENDING, 1U, UCN_OK }) &&
            ucn_cluster_persist_completion_is_valid(
                &(ucn_cluster_persist_completion_t){
                    UCN_CLUSTER_PERSIST_FAILED, 0U, UCN_ERR_STATE }) &&
            !ucn_cluster_persist_completion_is_valid(
                &(ucn_cluster_persist_completion_t){
                    UCN_CLUSTER_PERSIST_PENDING, 0U, UCN_OK }) &&
            !ucn_cluster_persist_completion_is_valid(
                &(ucn_cluster_persist_completion_t){ 0 }) &&
            transfer_config_v2.max_retries == 3U &&
            transfer_config_v2.ack_timeout_ms == UINT32_C(100) &&
            transfer_config_v2.fallback_rx_handler == NULL &&
           transfer_class_size_fn(UCN_TRANSFER_CLASS_T8K) == 8192U &&
           preset_fn(UCN_STANDARD_PRESET_UART_115200_8N1, NULL) == UCN_ERR_ARGUMENT &&
           bare_metal_init_fn(NULL, NULL) == UCN_ERR_ARGUMENT &&
           freertos_init_fn(NULL, NULL) == UCN_ERR_ARGUMENT &&
           zephyr_init_fn(NULL, NULL) == UCN_ERR_ARGUMENT &&
           nuttx_init_fn(NULL, NULL) == UCN_ERR_ARGUMENT &&
           rtthread_init_fn(NULL, NULL) == UCN_ERR_ARGUMENT &&
           host_fake_init_fn(NULL, NULL) == UCN_ERR_ARGUMENT &&
           event_runtime_init_fn(NULL, NULL) == UCN_ERR_ARGUMENT &&
           stream_source_init_fn(NULL, NULL) == UCN_ERR_ARGUMENT &&
            legacy_path.egress_link == &legacy_link &&
            legacy_path.expires_at_ms == UINT32_C(1000) &&
            path_install_capable_fn(NULL, NULL, &capability) == UCN_ERR_ARGUMENT &&
            !member_record_valid_fn(NULL) &&
            !member_table_valid_fn(NULL) && member_table_count_fn(NULL) == 0U &&
            !voter_set_valid_fn(NULL) && voter_set_quorum_fn(NULL) == 0U &&
            cluster_member_at_fn(NULL, 0U, NULL) == UCN_ERR_ARGUMENT &&
            cluster_member_capacity_fn(NULL, NULL) == UCN_ERR_ARGUMENT &&
            federation_init_fn(NULL, NULL) == UCN_ERR_ARGUMENT &&
            federation_receive_fn(NULL, 1U, false, NULL, 0U) == UCN_ERR_ARGUMENT &&
            federation_step_fn(NULL) == UCN_ERR_ARGUMENT &&
            federation_query_fn(NULL, 1U) == UCN_ERR_ARGUMENT &&
            federation_send_fn(NULL, 1U, 0x40U, UCN_TRAFFIC_Q1_REALTIME,
                               NULL, 0U) == UCN_ERR_ARGUMENT &&
            federation_find_fn(NULL, 1U) == NULL &&
            federation_next_fn(NULL, 1U) == NULL &&
            federation_stats_fn(NULL) == NULL &&
            ucn_node_install_local_path_capable(
               node, 1U, 2U, 2U, 1U, 1000U, &capability) == UCN_ERR_ARGUMENT &&
           ucn_node_send_path_install_capable(
               node, 2U, 1U, 2U, 0U, 0U, 1000U,
               &capability) == UCN_ERR_ARGUMENT ? 0 : 1;
}
