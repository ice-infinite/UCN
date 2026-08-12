#include <string.h>

#include "ucn/ucn_standard_adapter.h"

typedef struct ucn_standard_preset_entry {
    ucn_standard_preset_profile_t profile;
} ucn_standard_preset_entry_t;

#define PRESET_ENTRY(id, kind, rate, cost, rtt, mtu, preset_flags) \
    [id] = { { id, kind, rate, cost, rtt, mtu, preset_flags } }

#define UART_PRESET(id, rate, cost) \
    PRESET_ENTRY(id, UCN_BEARER_UART, rate, cost, 5U, \
                 UCN_MAX_FRAME_BYTES, UCN_STANDARD_PRESET_FLAG_NONE)

#define RS485_PRESET(id, rate, cost) \
    PRESET_ENTRY(id, UCN_BEARER_RS485, rate, cost, 5U, \
                 UCN_MAX_FRAME_BYTES, UCN_STANDARD_PRESET_FLAG_NONE)

#define CONDITIONAL_PRESET(id, kind, rate, cost, rtt) \
    PRESET_ENTRY(id, kind, rate, cost, rtt, UCN_MAX_FRAME_BYTES, \
                 UCN_STANDARD_PRESET_FLAG_CONDITIONAL)

static const ucn_standard_preset_entry_t STANDARD_PRESETS[
    UCN_STANDARD_PRESET_COUNT] = {
    UART_PRESET(UCN_STANDARD_PRESET_UART_9600_8N1, UINT32_C(9600), 140U),
    UART_PRESET(UCN_STANDARD_PRESET_UART_19200_8N1, UINT32_C(19200), 92U),
    UART_PRESET(UCN_STANDARD_PRESET_UART_38400_8N1, UINT32_C(38400), 62U),
    UART_PRESET(UCN_STANDARD_PRESET_UART_57600_8N1, UINT32_C(57600), 50U),
    UART_PRESET(UCN_STANDARD_PRESET_UART_115200_8N1, UINT32_C(115200), 34U),
    UART_PRESET(UCN_STANDARD_PRESET_UART_230400_8N1, UINT32_C(230400), 24U),
    UART_PRESET(UCN_STANDARD_PRESET_UART_460800_8N1, UINT32_C(460800), 17U),
    UART_PRESET(UCN_STANDARD_PRESET_UART_921600_8N1, UINT32_C(921600), 12U),
    UART_PRESET(UCN_STANDARD_PRESET_UART_1M_8N1, UINT32_C(1000000), 11U),
    UART_PRESET(UCN_STANDARD_PRESET_UART_2M_8N1, UINT32_C(2000000), 8U),
    UART_PRESET(UCN_STANDARD_PRESET_UART_3M_8N1, UINT32_C(3000000), 7U),
    UART_PRESET(UCN_STANDARD_PRESET_UART_4M_8N1, UINT32_C(4000000), 6U),

    RS485_PRESET(UCN_STANDARD_PRESET_RS485_9600_8N1, UINT32_C(9600), 152U),
    RS485_PRESET(UCN_STANDARD_PRESET_RS485_19200_8N1, UINT32_C(19200), 104U),
    RS485_PRESET(UCN_STANDARD_PRESET_RS485_38400_8N1, UINT32_C(38400), 74U),
    RS485_PRESET(UCN_STANDARD_PRESET_RS485_57600_8N1, UINT32_C(57600), 62U),
    RS485_PRESET(UCN_STANDARD_PRESET_RS485_115200_8N1, UINT32_C(115200), 46U),
    RS485_PRESET(UCN_STANDARD_PRESET_RS485_230400_8N1, UINT32_C(230400), 36U),
    RS485_PRESET(UCN_STANDARD_PRESET_RS485_460800_8N1, UINT32_C(460800), 29U),
    RS485_PRESET(UCN_STANDARD_PRESET_RS485_921600_8N1, UINT32_C(921600), 24U),
    RS485_PRESET(UCN_STANDARD_PRESET_RS485_1M_8N1, UINT32_C(1000000), 23U),
    RS485_PRESET(UCN_STANDARD_PRESET_RS485_2M_8N1, UINT32_C(2000000), 20U),
    RS485_PRESET(UCN_STANDARD_PRESET_RS485_3M_8N1, UINT32_C(3000000), 19U),
    RS485_PRESET(UCN_STANDARD_PRESET_RS485_4M_8N1, UINT32_C(4000000), 18U),

    PRESET_ENTRY(UCN_STANDARD_PRESET_CAN_CLASSIC_125K,
                 UCN_BEARER_CAN_CLASSIC, UINT32_C(125000), 110U, 3U,
                 UCN_MAX_FRAME_BYTES,
                 UCN_STANDARD_PRESET_FLAG_REQUIRES_CARRIER),
    PRESET_ENTRY(UCN_STANDARD_PRESET_CAN_CLASSIC_250K,
                 UCN_BEARER_CAN_CLASSIC, UINT32_C(250000), 72U, 3U,
                 UCN_MAX_FRAME_BYTES,
                 UCN_STANDARD_PRESET_FLAG_REQUIRES_CARRIER),
    PRESET_ENTRY(UCN_STANDARD_PRESET_CAN_CLASSIC_500K,
                 UCN_BEARER_CAN_CLASSIC, UINT32_C(500000), 45U, 3U,
                 UCN_MAX_FRAME_BYTES,
                 UCN_STANDARD_PRESET_FLAG_REQUIRES_CARRIER),
    PRESET_ENTRY(UCN_STANDARD_PRESET_CAN_CLASSIC_1M,
                 UCN_BEARER_CAN_CLASSIC, UINT32_C(1000000), 30U, 3U,
                 UCN_MAX_FRAME_BYTES,
                 UCN_STANDARD_PRESET_FLAG_REQUIRES_CARRIER),

    PRESET_ENTRY(UCN_STANDARD_PRESET_CAN_FD_500K_2M,
                 UCN_BEARER_CAN_FD, UINT32_C(2000000), 22U, 3U, 64U,
                 UCN_STANDARD_PRESET_FLAG_NONE),
    PRESET_ENTRY(UCN_STANDARD_PRESET_CAN_FD_500K_4M,
                 UCN_BEARER_CAN_FD, UINT32_C(4000000), 15U, 3U, 64U,
                 UCN_STANDARD_PRESET_FLAG_NONE),
    PRESET_ENTRY(UCN_STANDARD_PRESET_CAN_FD_1M_2M,
                 UCN_BEARER_CAN_FD, UINT32_C(2000000), 18U, 3U, 64U,
                 UCN_STANDARD_PRESET_FLAG_NONE),
    PRESET_ENTRY(UCN_STANDARD_PRESET_CAN_FD_1M_4M,
                 UCN_BEARER_CAN_FD, UINT32_C(4000000), 12U, 3U, 64U,
                 UCN_STANDARD_PRESET_FLAG_NONE),
    PRESET_ENTRY(UCN_STANDARD_PRESET_CAN_FD_1M_8M,
                 UCN_BEARER_CAN_FD, UINT32_C(8000000), 9U, 3U, 64U,
                 UCN_STANDARD_PRESET_FLAG_NONE),

    PRESET_ENTRY(UCN_STANDARD_PRESET_ESPNOW_DEFAULT_1M,
                 UCN_BEARER_WIFI, UINT32_C(1000000), 45U, 12U, 250U,
                 UCN_STANDARD_PRESET_FLAG_NONE),
    PRESET_ENTRY(UCN_STANDARD_PRESET_WIFI_1M,
                 UCN_BEARER_WIFI, UINT32_C(1000000), 52U, 12U,
                 UCN_MAX_FRAME_BYTES, UCN_STANDARD_PRESET_FLAG_NONE),
    PRESET_ENTRY(UCN_STANDARD_PRESET_WIFI_2M,
                 UCN_BEARER_WIFI, UINT32_C(2000000), 42U, 12U,
                 UCN_MAX_FRAME_BYTES, UCN_STANDARD_PRESET_FLAG_NONE),
    PRESET_ENTRY(UCN_STANDARD_PRESET_WIFI_6M,
                 UCN_BEARER_WIFI, UINT32_C(6000000), 30U, 12U,
                 UCN_MAX_FRAME_BYTES, UCN_STANDARD_PRESET_FLAG_NONE),
    PRESET_ENTRY(UCN_STANDARD_PRESET_WIFI_12M,
                 UCN_BEARER_WIFI, UINT32_C(12000000), 24U, 12U,
                 UCN_MAX_FRAME_BYTES, UCN_STANDARD_PRESET_FLAG_NONE),
    PRESET_ENTRY(UCN_STANDARD_PRESET_WIFI_24M,
                 UCN_BEARER_WIFI, UINT32_C(24000000), 18U, 12U,
                 UCN_MAX_FRAME_BYTES, UCN_STANDARD_PRESET_FLAG_NONE),
    PRESET_ENTRY(UCN_STANDARD_PRESET_WIFI_54M,
                 UCN_BEARER_WIFI, UINT32_C(54000000), 14U, 12U,
                 UCN_MAX_FRAME_BYTES, UCN_STANDARD_PRESET_FLAG_NONE),
    PRESET_ENTRY(UCN_STANDARD_PRESET_WIFI_UNKNOWN_RATE,
                 UCN_BEARER_WIFI, 0U, 45U, 12U, UCN_MAX_FRAME_BYTES,
                 UCN_STANDARD_PRESET_FLAG_NONE),

    PRESET_ENTRY(UCN_STANDARD_PRESET_USB_CDC_FS,
                 UCN_BEARER_USB_CDC, UINT32_C(12000000), 14U, 4U,
                 UCN_MAX_FRAME_BYTES, UCN_STANDARD_PRESET_FLAG_NONE),
    PRESET_ENTRY(UCN_STANDARD_PRESET_USB_CDC_HS,
                 UCN_BEARER_USB_CDC, UINT32_C(480000000), 6U, 4U,
                 UCN_MAX_FRAME_BYTES, UCN_STANDARD_PRESET_FLAG_NONE),

    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_ETHERNET_10M,
                       UCN_BEARER_ETHERNET, UINT32_C(10000000), 10U, 4U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_ETHERNET_100M,
                       UCN_BEARER_ETHERNET, UINT32_C(100000000), 8U, 4U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_ETHERNET_1G,
                       UCN_BEARER_ETHERNET, UINT32_C(1000000000), 5U, 4U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_BLE_LE_1M,
                       UCN_BEARER_BLE_LE, UINT32_C(1000000), 65U, 12U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_BLE_LE_2M,
                       UCN_BEARER_BLE_LE, UINT32_C(2000000), 48U, 12U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_BLE_LE_CODED_S2,
                       UCN_BEARER_BLE_LE, UINT32_C(500000), 160U, 12U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_BLE_LE_CODED_S8,
                       UCN_BEARER_BLE_LE, UINT32_C(125000), 360U, 12U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_IEEE_802154_250K,
                       UCN_BEARER_IEEE_802154, UINT32_C(250000), 75U, 12U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_PRIVATE_2G4_250K,
                       UCN_BEARER_PRIVATE_2G4, UINT32_C(250000), 120U, 12U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_PRIVATE_2G4_1M,
                       UCN_BEARER_PRIVATE_2G4, UINT32_C(1000000), 70U, 12U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_PRIVATE_2G4_2M,
                       UCN_BEARER_PRIVATE_2G4, UINT32_C(2000000), 50U, 12U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_FSK_50K,
                       UCN_BEARER_FSK_SUB_GHZ, UINT32_C(50000), 260U, 80U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_FSK_100K,
                       UCN_BEARER_FSK_SUB_GHZ, UINT32_C(100000), 170U, 80U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_FSK_250K,
                       UCN_BEARER_FSK_SUB_GHZ, UINT32_C(250000), 95U, 80U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_LORA_SF7_BW250K,
                       UCN_BEARER_LORA_P2P, 0U, 180U, 80U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_LORA_SF7_BW125K,
                       UCN_BEARER_LORA_P2P, 0U, 240U, 80U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_LORA_SF9_BW125K,
                       UCN_BEARER_LORA_P2P, 0U, 420U, 80U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_LORA_SF12_BW125K,
                       UCN_BEARER_LORA_P2P, 0U, 820U, 80U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_UWB_850K,
                       UCN_BEARER_UWB, UINT32_C(850000), 120U, 10U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_UWB_6M8,
                       UCN_BEARER_UWB, UINT32_C(6800000), 55U, 10U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_SPI_1M,
                       UCN_BEARER_SPI, UINT32_C(1000000), 20U, 4U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_SPI_4M,
                       UCN_BEARER_SPI, UINT32_C(4000000), 12U, 4U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_SPI_10M,
                       UCN_BEARER_SPI, UINT32_C(10000000), 8U, 4U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_I2C_100K,
                       UCN_BEARER_I2C, UINT32_C(100000), 120U, 4U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_I2C_400K,
                       UCN_BEARER_I2C, UINT32_C(400000), 70U, 4U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_I2C_1M,
                       UCN_BEARER_I2C, UINT32_C(1000000), 45U, 4U),
    CONDITIONAL_PRESET(UCN_STANDARD_PRESET_IP_TUNNEL_LAN_UDP,
                       UCN_BEARER_IP_TUNNEL, 0U, 25U, 4U)
};

static bool standard_preset_is_valid(ucn_standard_preset_t preset)
{
    return preset > UCN_STANDARD_PRESET_UNSPECIFIED &&
           preset < UCN_STANDARD_PRESET_COUNT &&
           STANDARD_PRESETS[preset].profile.preset == preset;
}

ucn_result_t ucn_standard_preset_resolve(
    ucn_standard_preset_t preset,
    ucn_standard_preset_profile_t *profile)
{
    if (profile == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!standard_preset_is_valid(preset)) {
        return UCN_ERR_CONFIG;
    }

    *profile = STANDARD_PRESETS[preset].profile;
    return UCN_OK;
}

ucn_result_t ucn_standard_link_config_resolve(
    const ucn_standard_link_config_t *config,
    ucn_standard_resolved_link_config_t *resolved)
{
    ucn_standard_preset_profile_t profile;
    size_t logical_mtu;
    ucn_result_t result;

    if (config == NULL || resolved == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (config->local_link_id == 0U ||
        config->peer_node_id == UCN_NODE_BROADCAST ||
        config->peer_address.length > UCN_ADAPTER_PHYSICAL_ADDRESS_MAX ||
        (config->peer_address.length != 0U &&
         !ucn_adapter_address_is_valid(&config->peer_address)) ||
        config->administrative_bias < UCN_STANDARD_ADMINISTRATIVE_BIAS_MIN ||
        config->administrative_bias > UCN_STANDARD_ADMINISTRATIVE_BIAS_MAX) {
        return UCN_ERR_CONFIG;
    }

    result = ucn_standard_preset_resolve(config->preset, &profile);
    if (result != UCN_OK) {
        return result;
    }
    if ((profile.flags & UCN_STANDARD_PRESET_FLAG_REQUIRES_CARRIER) != 0U &&
        !config->carrier_enabled) {
        return UCN_ERR_CONFIG;
    }

    logical_mtu = config->required_logical_mtu == UCN_STANDARD_LOGICAL_MTU_AUTO ?
                      UCN_MAX_FRAME_BYTES : config->required_logical_mtu;
    if (logical_mtu < UCN_STANDARD_LOGICAL_MTU_MIN ||
        logical_mtu > profile.maximum_logical_mtu) {
        return UCN_ERR_CONFIG;
    }
    if (config->override_base_cost &&
        (config->base_cost_override == 0U ||
         config->base_cost_override == UCN_LINK_ROUTE_COST_UNKNOWN)) {
        return UCN_ERR_CONFIG;
    }
    if (config->override_rtt_reference &&
        config->rtt_reference_ms_override == 0U) {
        return UCN_ERR_CONFIG;
    }

    (void)memset(resolved, 0, sizeof(*resolved));
    resolved->profile = profile;
    resolved->local_link_id = config->local_link_id;
    resolved->peer_node_id = config->peer_node_id;
    resolved->peer_address = config->peer_address;
    resolved->logical_mtu = logical_mtu;
    resolved->base_cost = config->override_base_cost ?
                              config->base_cost_override : profile.base_cost;
    resolved->rtt_reference_ms = config->override_rtt_reference ?
                                      config->rtt_reference_ms_override :
                                      profile.rtt_reference_ms;
    resolved->administrative_bias = config->administrative_bias;
    return UCN_OK;
}
