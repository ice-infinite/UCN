#include "ucn/v6/adapters/ucn_v6_wifi.h"

#include <string.h>

static uint8_t quota(uint8_t configured, size_t total)
{
    size_t value = configured != 0U ? configured :
        total / UCN_V6_CONFIG_ADAPTER_LINKS;
    if (value == 0U) value = 1U;
    return (uint8_t)value;
}

ucn_v6_result_t ucn_v6_esp_now_link_config_init(
    const ucn_v6_esp_now_link_settings_t *settings,
    ucn_v6_driver_link_config_t *config)
{
    ucn_v6_driver_link_config_t next;
    uint16_t carrier_mtu;
    if (settings == NULL || config == NULL || settings->base.link_id == 0U ||
        settings->base.initial_generation == 0U ||
        settings->base.initial_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_ARGUMENT;
    }
    carrier_mtu = settings->carrier_payload_mtu != 0U ?
        settings->carrier_payload_mtu :
        (UCN_V6_CONFIG_ADAPTER_FRAME_BYTES < 250U ?
             UCN_V6_CONFIG_ADAPTER_FRAME_BYTES : 250U);
    if (carrier_mtu > UCN_V6_CONFIG_ADAPTER_FRAME_BYTES) {
        return UCN_V6_ERR_CONFIG;
    }
    memset(&next, 0, sizeof(next));
    next.link_id = settings->base.link_id;
    next.initial_generation = settings->base.initial_generation;
    next.bearer = UCN_V6_BEARER_ESP_NOW;
    next.nominal_bitrate_bps = settings->effective_bitrate_bps != 0U ?
        settings->effective_bitrate_bps : UINT32_C(1000000);
    next.carrier_mtu = carrier_mtu;
    next.link_frame_mtu = UCN_V6_CONFIG_ADAPTER_FRAME_BYTES;
    next.hardware_priority_count = 1U;
    next.rx_slot_quota = quota(settings->base.rx_slot_quota,
                               UCN_V6_CONFIG_ADAPTER_RX_SLOTS);
    next.tx_slot_quota = quota(settings->base.tx_slot_quota,
                               UCN_V6_CONFIG_ADAPTER_TX_SLOTS);
    next.rx_timestamp_hardware = settings->rx_timestamp_hardware;
    next.tx_timestamp_hardware = settings->tx_timestamp_hardware;
    next.ops = settings->base.ops;
    *config = next;
    return UCN_V6_OK;
}
