#ifndef UCN_V6_ADAPTER_PRIVATE_H
#define UCN_V6_ADAPTER_PRIVATE_H

#include "ucn/v6/ucn_v6_adapter.h"

#define UCN_V6_ADAPTER_MAGIC UINT32_C(0x56364144)
#define UCN_V6_ADAPTER_SCHEMA UINT16_C(1)
#define UCN_V6_ADAPTER_CANARY UINT64_C(0x5636414450545252)

typedef struct ucn_v6_adapter_link_slot {
    bool occupied;
    bool io_active;
    uint8_t rx_count;
    uint8_t tx_count;
    ucn_v6_driver_link_readiness_t readiness;
    ucn_v6_driver_link_config_t config;
} ucn_v6_adapter_link_slot_t;

typedef struct ucn_v6_adapter_rx_slot {
    bool occupied;
    uint64_t order;
    ucn_v6_driver_event_key_t key;
    ucn_v6_driver_timestamp_t timestamp;
    ucn_v6_bearer_kind_t bearer;
    uint16_t frame_length;
    uint8_t frame[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
} ucn_v6_adapter_rx_slot_t;

typedef struct ucn_v6_adapter_tx_slot {
    bool occupied;
    uint64_t order;
    ucn_v6_driver_event_key_t key;
    ucn_v6_driver_timestamp_t timestamp;
    uint64_t buffer_token;
    ucn_v6_result_t completion;
    ucn_v6_driver_tx_state_t state;
    ucn_v6_traffic_class_t traffic_class;
    uint16_t frame_length;
    bool request_timestamp;
    uint8_t frame[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
} ucn_v6_adapter_tx_slot_t;

struct ucn_v6_adapter_owner {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    ucn_v6_driver_runtime_ops_t runtime;
    ucn_v6_adapter_link_slot_t links[UCN_V6_CONFIG_ADAPTER_LINKS];
    ucn_v6_adapter_rx_slot_t rx[UCN_V6_CONFIG_ADAPTER_RX_SLOTS];
    ucn_v6_adapter_tx_slot_t tx[UCN_V6_CONFIG_ADAPTER_TX_SLOTS];
    uint64_t next_order;
    uint32_t next_event_token;
    ucn_v6_adapter_stats_t stats;
    bool initialized;
    uint64_t canary;
};

#endif
