#ifndef UCN_STANDARD_ADAPTER_H
#define UCN_STANDARD_ADAPTER_H

/*
 * SDK-independent, static configuration vocabulary for future standard UCN
 * Port/Adapter implementations.  This header deliberately does not contain
 * GPIO numbers, FreeRTOS/Zephyr types, driver handles or callbacks: those
 * remain product/BSP concerns.  It only resolves a declared media preset into
 * the stable Link facts that an Adapter must later report consistently.
 */
#include "ucn/ucn_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_STANDARD_ADMINISTRATIVE_BIAS_MIN ((int16_t)-32)
#define UCN_STANDARD_ADMINISTRATIVE_BIAS_MAX ((int16_t)64)
#define UCN_STANDARD_LOGICAL_MTU_AUTO ((size_t)0U)
#define UCN_STANDARD_LOGICAL_MTU_MIN \
    (UCN_FRAME_W0_HEADER_SIZE + (size_t)1U)

typedef enum ucn_bearer_kind {
    UCN_BEARER_UNSPECIFIED = 0,
    UCN_BEARER_UART = 1,
    UCN_BEARER_RS485 = 2,
    UCN_BEARER_CAN_CLASSIC = 3,
    UCN_BEARER_CAN_FD = 4,
    UCN_BEARER_WIFI = 5,
    UCN_BEARER_USB_CDC = 6,
    UCN_BEARER_ETHERNET = 7,
    UCN_BEARER_BLE_LE = 8,
    UCN_BEARER_IEEE_802154 = 9,
    UCN_BEARER_PRIVATE_2G4 = 10,
    UCN_BEARER_FSK_SUB_GHZ = 11,
    UCN_BEARER_LORA_P2P = 12,
    UCN_BEARER_UWB = 13,
    UCN_BEARER_SPI = 14,
    UCN_BEARER_I2C = 15,
    UCN_BEARER_IP_TUNNEL = 16
} ucn_bearer_kind_t;

/* A preset with a rate of zero is not an invalid profile: it denotes a
 * condition-defined medium (for example LoRa SF/BW or a LAN Tunnel) for
 * which there is intentionally no single comparable bit-rate number. */
typedef enum ucn_standard_preset {
    UCN_STANDARD_PRESET_UNSPECIFIED = 0,
    UCN_STANDARD_PRESET_UART_9600_8N1,
    UCN_STANDARD_PRESET_UART_19200_8N1,
    UCN_STANDARD_PRESET_UART_38400_8N1,
    UCN_STANDARD_PRESET_UART_57600_8N1,
    UCN_STANDARD_PRESET_UART_115200_8N1,
    UCN_STANDARD_PRESET_UART_230400_8N1,
    UCN_STANDARD_PRESET_UART_460800_8N1,
    UCN_STANDARD_PRESET_UART_921600_8N1,
    UCN_STANDARD_PRESET_UART_1M_8N1,
    UCN_STANDARD_PRESET_UART_2M_8N1,
    UCN_STANDARD_PRESET_UART_3M_8N1,
    UCN_STANDARD_PRESET_UART_4M_8N1,
    UCN_STANDARD_PRESET_RS485_9600_8N1,
    UCN_STANDARD_PRESET_RS485_19200_8N1,
    UCN_STANDARD_PRESET_RS485_38400_8N1,
    UCN_STANDARD_PRESET_RS485_57600_8N1,
    UCN_STANDARD_PRESET_RS485_115200_8N1,
    UCN_STANDARD_PRESET_RS485_230400_8N1,
    UCN_STANDARD_PRESET_RS485_460800_8N1,
    UCN_STANDARD_PRESET_RS485_921600_8N1,
    UCN_STANDARD_PRESET_RS485_1M_8N1,
    UCN_STANDARD_PRESET_RS485_2M_8N1,
    UCN_STANDARD_PRESET_RS485_3M_8N1,
    UCN_STANDARD_PRESET_RS485_4M_8N1,
    UCN_STANDARD_PRESET_CAN_CLASSIC_125K,
    UCN_STANDARD_PRESET_CAN_CLASSIC_250K,
    UCN_STANDARD_PRESET_CAN_CLASSIC_500K,
    UCN_STANDARD_PRESET_CAN_CLASSIC_1M,
    UCN_STANDARD_PRESET_CAN_FD_500K_2M,
    UCN_STANDARD_PRESET_CAN_FD_500K_4M,
    UCN_STANDARD_PRESET_CAN_FD_1M_2M,
    UCN_STANDARD_PRESET_CAN_FD_1M_4M,
    UCN_STANDARD_PRESET_CAN_FD_1M_8M,
    UCN_STANDARD_PRESET_ESPNOW_DEFAULT_1M,
    UCN_STANDARD_PRESET_WIFI_1M,
    UCN_STANDARD_PRESET_WIFI_2M,
    UCN_STANDARD_PRESET_WIFI_6M,
    UCN_STANDARD_PRESET_WIFI_12M,
    UCN_STANDARD_PRESET_WIFI_24M,
    UCN_STANDARD_PRESET_WIFI_54M,
    UCN_STANDARD_PRESET_WIFI_UNKNOWN_RATE,
    UCN_STANDARD_PRESET_USB_CDC_FS,
    UCN_STANDARD_PRESET_USB_CDC_HS,
    UCN_STANDARD_PRESET_ETHERNET_10M,
    UCN_STANDARD_PRESET_ETHERNET_100M,
    UCN_STANDARD_PRESET_ETHERNET_1G,
    UCN_STANDARD_PRESET_BLE_LE_1M,
    UCN_STANDARD_PRESET_BLE_LE_2M,
    UCN_STANDARD_PRESET_BLE_LE_CODED_S2,
    UCN_STANDARD_PRESET_BLE_LE_CODED_S8,
    UCN_STANDARD_PRESET_IEEE_802154_250K,
    UCN_STANDARD_PRESET_PRIVATE_2G4_250K,
    UCN_STANDARD_PRESET_PRIVATE_2G4_1M,
    UCN_STANDARD_PRESET_PRIVATE_2G4_2M,
    UCN_STANDARD_PRESET_FSK_50K,
    UCN_STANDARD_PRESET_FSK_100K,
    UCN_STANDARD_PRESET_FSK_250K,
    UCN_STANDARD_PRESET_LORA_SF7_BW250K,
    UCN_STANDARD_PRESET_LORA_SF7_BW125K,
    UCN_STANDARD_PRESET_LORA_SF9_BW125K,
    UCN_STANDARD_PRESET_LORA_SF12_BW125K,
    UCN_STANDARD_PRESET_UWB_850K,
    UCN_STANDARD_PRESET_UWB_6M8,
    UCN_STANDARD_PRESET_SPI_1M,
    UCN_STANDARD_PRESET_SPI_4M,
    UCN_STANDARD_PRESET_SPI_10M,
    UCN_STANDARD_PRESET_I2C_100K,
    UCN_STANDARD_PRESET_I2C_400K,
    UCN_STANDARD_PRESET_I2C_1M,
    UCN_STANDARD_PRESET_IP_TUNNEL_LAN_UDP,
    UCN_STANDARD_PRESET_COUNT
} ucn_standard_preset_t;

typedef uint8_t ucn_standard_preset_flags_t;
enum {
    UCN_STANDARD_PRESET_FLAG_NONE = 0U,
    /* A classic CAN profile is usable only after an Adapter-owned bounded
     * segment/reassembly carrier has been selected and initialized. */
    UCN_STANDARD_PRESET_FLAG_REQUIRES_CARRIER = 1U << 0,
    /* This value is frozen by LC-1 but is not an implemented Adapter.  The
     * flag is diagnostics/configuration metadata, not a runtime block. */
    UCN_STANDARD_PRESET_FLAG_CONDITIONAL = 1U << 1
};

typedef struct ucn_standard_preset_profile {
    ucn_standard_preset_t preset;
    ucn_bearer_kind_t bearer_kind;
    uint32_t configured_rate_bps;
    uint16_t base_cost;
    uint16_t rtt_reference_ms;
    size_t maximum_logical_mtu;
    ucn_standard_preset_flags_t flags;
} ucn_standard_preset_profile_t;

/* Product-owned immutable input.  A zero peer Node ID/physical address is a
 * dynamic-discovery Candidate Link.  required_logical_mtu=0 inherits the
 * library maximum; a nonzero value explicitly restricts the Link below the
 * preset capability.  This resolver never mutates ucn_link_t or a driver. */
typedef struct ucn_standard_link_config {
    uint8_t local_link_id;
    ucn_node_id_t peer_node_id;
    ucn_adapter_address_t peer_address;
    ucn_standard_preset_t preset;
    size_t required_logical_mtu;
    bool carrier_enabled;
    bool override_base_cost;
    uint16_t base_cost_override;
    bool override_rtt_reference;
    uint16_t rtt_reference_ms_override;
    int16_t administrative_bias;
} ucn_standard_link_config_t;

typedef struct ucn_standard_resolved_link_config {
    ucn_standard_preset_profile_t profile;
    uint8_t local_link_id;
    ucn_node_id_t peer_node_id;
    ucn_adapter_address_t peer_address;
    size_t logical_mtu;
    uint16_t base_cost;
    uint16_t rtt_reference_ms;
    int16_t administrative_bias;
} ucn_standard_resolved_link_config_t;

/* Return UCN_ERR_CONFIG for UNSPECIFIED or an unknown enum value. */
ucn_result_t ucn_standard_preset_resolve(
    ucn_standard_preset_t preset,
    ucn_standard_preset_profile_t *profile);

/* Apply the explicit priority contract:
 * preset base/RTT/MTU -> per-Link base/RTT/MTU override.  base overrides are
 * valid only in 1..UCN_LINK_ROUTE_COST_MAX; a requested MTU must be at least
 * UCN_STANDARD_LOGICAL_MTU_MIN and no greater than the preset capability.
 * No dynamic link-quality or driver state is consulted here. */
ucn_result_t ucn_standard_link_config_resolve(
    const ucn_standard_link_config_t *config,
    ucn_standard_resolved_link_config_t *resolved);

#ifdef __cplusplus
}
#endif

#endif
