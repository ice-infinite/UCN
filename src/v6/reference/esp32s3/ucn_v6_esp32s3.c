#include "ucn/v6/reference/esp32s3/ucn_v6_esp32s3.h"

static bool gpio_valid(int8_t gpio)
{
    return gpio >= 0 && gpio <= 48;
}

ucn_v6_result_t ucn_v6_esp32s3_uart_binding_build(
    const ucn_v6_esp32s3_uart_binding_t *binding,
    ucn_v6_driver_link_config_t *config)
{
    if (binding == NULL || config == NULL ||
        binding->struct_size != sizeof(*binding) ||
        binding->api_version != UCN_V6_ESP32S3_REFERENCE_API_VERSION ||
        binding->uart_port > 2U || !gpio_valid(binding->tx_gpio) ||
        !gpio_valid(binding->rx_gpio) ||
        binding->tx_gpio == binding->rx_gpio ||
        binding->rx_dma_bytes < 256U || binding->tx_dma_bytes < 256U ||
        (binding->link.rs485 && !gpio_valid(binding->rts_or_de_gpio)) ||
        (!binding->link.rs485 &&
         binding->rts_or_de_gpio != UCN_V6_ESP32S3_GPIO_UNUSED &&
         !gpio_valid(binding->rts_or_de_gpio))) {
        return UCN_V6_ERR_CONFIG;
    }
    return ucn_v6_uart_link_config_init(&binding->link, config);
}

ucn_v6_result_t ucn_v6_esp32s3_esp_now_binding_build(
    const ucn_v6_esp32s3_esp_now_binding_t *binding,
    ucn_v6_driver_link_config_t *config)
{
    if (binding == NULL || config == NULL ||
        binding->struct_size != sizeof(*binding) ||
        binding->api_version != UCN_V6_ESP32S3_REFERENCE_API_VERSION ||
        binding->wifi_interface > 1U || binding->channel < 1U ||
        binding->channel > 14U || binding->peer_capacity == 0U) {
        return UCN_V6_ERR_CONFIG;
    }
    return ucn_v6_esp_now_link_config_init(&binding->link, config);
}
