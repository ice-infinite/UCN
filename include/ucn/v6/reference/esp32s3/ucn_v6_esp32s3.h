#ifndef UCN_V6_ESP32S3_H
#define UCN_V6_ESP32S3_H

/* ESP32-S3 + FreeRTOS reference-product configuration.
 * This header intentionally contains no ESP-IDF types: one product BSP owns
 * GPIO selection and translates the validated result to the SDK. */

#include "ucn/v6/adapters/ucn_v6_uart.h"
#include "ucn/v6/adapters/ucn_v6_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_ESP32S3_REFERENCE_API_VERSION UINT16_C(1)
#define UCN_V6_ESP32S3_GPIO_UNUSED ((int8_t)-1)

typedef struct ucn_v6_esp32s3_uart_binding {
    size_t struct_size;
    uint16_t api_version;
    uint8_t uart_port;
    int8_t tx_gpio;
    int8_t rx_gpio;
    int8_t rts_or_de_gpio;
    uint16_t rx_dma_bytes;
    uint16_t tx_dma_bytes;
    ucn_v6_uart_link_settings_t link;
} ucn_v6_esp32s3_uart_binding_t;

typedef struct ucn_v6_esp32s3_esp_now_binding {
    size_t struct_size;
    uint16_t api_version;
    uint8_t wifi_interface;
    uint8_t channel;
    uint8_t peer_capacity;
    ucn_v6_esp_now_link_settings_t link;
} ucn_v6_esp32s3_esp_now_binding_t;

/* EN: Validates pins/DMA ownership and produces a generic UART Link config.
 * 中文：验证引脚和 DMA 所有权，并生成通用 UART Link 配置。 */
ucn_v6_result_t ucn_v6_esp32s3_uart_binding_build(
    const ucn_v6_esp32s3_uart_binding_t *binding,
    ucn_v6_driver_link_config_t *config);

/* EN: Validates channel/peer capacity and produces an ESP-NOW Link config.
 * 中文：验证信道与 Peer 容量，并生成 ESP-NOW Link 配置。 */
ucn_v6_result_t ucn_v6_esp32s3_esp_now_binding_build(
    const ucn_v6_esp32s3_esp_now_binding_t *binding,
    ucn_v6_driver_link_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
