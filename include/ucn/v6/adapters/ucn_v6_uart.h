#ifndef UCN_V6_UART_H
#define UCN_V6_UART_H

#include "ucn/v6/ucn_v6_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pins, UART controller, DMA channel and DE timing stay in the product BSP. */
typedef struct ucn_v6_uart_link_settings {
    ucn_v6_driver_link_base_t base;
    uint32_t bitrate_bps;
    bool rs485;
    bool rx_timestamp_hardware;
    bool tx_timestamp_hardware;
} ucn_v6_uart_link_settings_t;

/* EN: Builds UART/RS-485 Link capabilities without touching hardware.
 * 中文：构造 UART/RS-485 Link 能力，不访问硬件。 */
ucn_v6_result_t ucn_v6_uart_link_config_init(
    const ucn_v6_uart_link_settings_t *settings,
    ucn_v6_driver_link_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
