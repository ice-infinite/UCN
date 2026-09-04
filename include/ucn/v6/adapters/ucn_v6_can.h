#ifndef UCN_V6_CAN_H
#define UCN_V6_CAN_H

#include "ucn/v6/ucn_v6_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucn_v6_can_link_settings {
    ucn_v6_driver_link_base_t base;
    uint32_t arbitration_bitrate_bps;
    uint32_t data_bitrate_bps;
    bool can_fd;
    bool rx_timestamp_hardware;
    bool tx_timestamp_hardware;
} ucn_v6_can_link_settings_t;

/* EN: Builds Classic CAN or CAN-FD capabilities. Carrier segmentation remains
 * inside this Link driver and always delivers one complete UCN Link frame.
 * 中文：构造经典 CAN 或 CAN-FD 能力；Carrier 分片保留在 Link Driver 内，
 * 向上始终交付完整 UCN Link 帧。 */
ucn_v6_result_t ucn_v6_can_link_config_init(
    const ucn_v6_can_link_settings_t *settings,
    ucn_v6_driver_link_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
