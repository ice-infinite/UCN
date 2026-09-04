#ifndef UCN_V6_WIFI_H
#define UCN_V6_WIFI_H

#include "ucn/v6/ucn_v6_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucn_v6_esp_now_link_settings {
    ucn_v6_driver_link_base_t base;
    uint32_t effective_bitrate_bps;
    uint16_t carrier_payload_mtu;
    bool rx_timestamp_hardware;
    bool tx_timestamp_hardware;
} ucn_v6_esp_now_link_settings_t;

/* EN: Builds an ESP-NOW datagram Link; channel, peers and MAC live in BSP.
 * 中文：构造 ESP-NOW 数据报 Link；信道、Peer 与 MAC 由 BSP 管理。 */
ucn_v6_result_t ucn_v6_esp_now_link_config_init(
    const ucn_v6_esp_now_link_settings_t *settings,
    ucn_v6_driver_link_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
