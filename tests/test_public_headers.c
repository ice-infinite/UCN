#include <stddef.h>

#include "ucn/ucn_adapter.h"
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
    ucn_result_t (*transfer_local_window_fn)(ucn_transfer_t *, uint8_t) =
        ucn_transfer_set_tx_window_size;
    ucn_result_t (*transfer_step_fn)(ucn_transfer_t *) = ucn_transfer_step;

    return node == NULL && step_fn != NULL && stats_fn != NULL && preset_fn != NULL &&
           isr_enqueue_fn != NULL && path_install_capable_fn != NULL &&
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
            transfer_local_window_fn != NULL && transfer_step_fn != NULL &&
            ucn_port_ops_is_compatible(&port_ops_v2) &&
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
           ucn_node_install_local_path_capable(
               node, 1U, 2U, 2U, 1U, 1000U, &capability) == UCN_ERR_ARGUMENT &&
           ucn_node_send_path_install_capable(
               node, 2U, 1U, 2U, 0U, 0U, 1000U,
               &capability) == UCN_ERR_ARGUMENT ? 0 : 1;
}
