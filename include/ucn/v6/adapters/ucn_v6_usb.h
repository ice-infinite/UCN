#ifndef UCN_V6_USB_H
#define UCN_V6_USB_H

#include "ucn/v6/ucn_v6_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucn_v6_usb_link_settings {
    ucn_v6_driver_link_base_t base;
    uint32_t effective_bitrate_bps;
    uint16_t transfer_mtu;
    uint8_t hardware_priority_count;
    bool rx_timestamp_hardware;
    bool tx_timestamp_hardware;
} ucn_v6_usb_link_settings_t;

/* EN: Builds USB CDC/Bulk capabilities; endpoint allocation stays in BSP.
 * 中文：构造 USB CDC/Bulk 能力；Endpoint 分配由 BSP 管理。 */
ucn_v6_result_t ucn_v6_usb_link_config_init(
    const ucn_v6_usb_link_settings_t *settings,
    ucn_v6_driver_link_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
