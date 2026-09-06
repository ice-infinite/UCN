#include "ucn/v6/adapters/ucn_v6_usb.h"

#include <string.h>

static uint8_t quota(uint8_t configured, size_t total)
{
    size_t value = configured != 0U ? configured :
        total / UCN_V6_CONFIG_ADAPTER_LINKS;
    if (value == 0U) value = 1U;
    return (uint8_t)value;
}

ucn_v6_result_t ucn_v6_usb_link_config_init(
    const ucn_v6_usb_link_settings_t *settings,
    ucn_v6_driver_link_config_t *config)
{
    ucn_v6_driver_link_config_t next;
    uint16_t mtu;
    if (settings == NULL || config == NULL || settings->base.link_id == 0U ||
        settings->base.link_id > UCN_V6_LINK_ID_MAX ||
        settings->base.initial_generation == 0U ||
        settings->base.initial_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_ARGUMENT;
    }
    mtu = settings->transfer_mtu != 0U ? settings->transfer_mtu :
        (UCN_V6_CONFIG_ADAPTER_FRAME_BYTES < 512U ?
             UCN_V6_CONFIG_ADAPTER_FRAME_BYTES : 512U);
    if (mtu > UCN_V6_CONFIG_ADAPTER_FRAME_BYTES ||
        settings->hardware_priority_count > 8U) {
        return UCN_V6_ERR_CONFIG;
    }
    memset(&next, 0, sizeof(next));
    next.link_id = settings->base.link_id;
    next.initial_generation = settings->base.initial_generation;
    next.bearer = UCN_V6_BEARER_USB;
    next.nominal_bitrate_bps = settings->effective_bitrate_bps != 0U ?
        settings->effective_bitrate_bps : UINT32_C(12000000);
    next.carrier_mtu = mtu;
    next.link_frame_mtu = UCN_V6_CONFIG_ADAPTER_FRAME_BYTES;
    next.hardware_priority_count = settings->hardware_priority_count != 0U ?
        settings->hardware_priority_count : 4U;
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
