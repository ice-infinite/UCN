#ifndef UCN_V6_TRANSFER_PRIVATE_H
#define UCN_V6_TRANSFER_PRIVATE_H

#include "ucn/v6/ucn_v6_transfer.h"

#define UCN_V6_TRANSFER_OWNER_MAGIC UINT32_C(0x56545236)
#define UCN_V6_TRANSFER_OWNER_CANARY UINT64_C(0xA95E13C27B6048DF)
#define UCN_V6_TRANSFER_MAX_FRAGMENTS UCN_V6_TRANSFER_MAX_MESSAGE_BYTES
#define UCN_V6_TRANSFER_BITMAP_BYTES \
    ((UCN_V6_TRANSFER_MAX_FRAGMENTS + 7U) / 8U)

typedef struct ucn_v6_transfer_tx_fragment_state {
    uint16_t fragment_index;
    uint8_t attempts;
    bool sent;
    bool acknowledged;
    uint64_t last_submit_us;
} ucn_v6_transfer_tx_fragment_state_t;

typedef struct ucn_v6_transfer_tx_slot {
    bool occupied;
    ucn_v6_transfer_tx_phase_t phase;
    ucn_v6_transfer_send_request_t request;
    uint16_t fragment_count;
    uint16_t cumulative_base;
    uint32_t message_crc32c;
    ucn_v6_transfer_tx_fragment_state_t
        window[UCN_V6_CONFIG_TRANSFER_WINDOW];
} ucn_v6_transfer_tx_slot_t;

typedef struct ucn_v6_transfer_rx_slot {
    bool occupied;
    bool complete;
    ucn_v6_session_key_t origin;
    uint64_t operation_id;
    uint64_t message_id;
    ucn_v6_message_class_t message_class;
    uint16_t total_length;
    uint16_t fragment_count;
    uint16_t fragment_data_budget;
    uint16_t received_count;
    uint32_t message_crc32c;
    uint64_t deadline_us;
    uint8_t received[UCN_V6_TRANSFER_BITMAP_BYTES];
    uint8_t data[UCN_V6_TRANSFER_MAX_MESSAGE_BYTES];
} ucn_v6_transfer_rx_slot_t;

typedef struct ucn_v6_transfer_recent_slot {
    bool occupied;
    ucn_v6_session_key_t origin;
    uint64_t operation_id;
    uint64_t message_id;
    ucn_v6_message_class_t message_class;
    uint16_t total_length;
    uint16_t fragment_count;
    uint16_t fragment_data_budget;
    uint32_t message_crc32c;
    uint64_t deadline_us;
} ucn_v6_transfer_recent_slot_t;

typedef struct ucn_v6_transfer_credit_slot {
    bool occupied;
    ucn_v6_session_key_t peer;
    ucn_v6_transfer_credit_update_t update;
    uint64_t deadline_us;
} ucn_v6_transfer_credit_slot_t;

typedef struct ucn_v6_transfer_credit_reservation_slot {
    bool occupied;
    ucn_v6_transfer_credit_reservation_t value;
} ucn_v6_transfer_credit_reservation_slot_t;

struct ucn_v6_transfer_owner {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    uint64_t retry_interval_us;
    uint8_t fragment_max_attempts;
    uint64_t receive_timeout_us;
    uint64_t recent_completion_us;
    uint64_t next_credit_reservation_id;
    ucn_v6_transfer_tx_slot_t tx[UCN_V6_CONFIG_TRANSFER_TX_SLOTS];
    ucn_v6_transfer_rx_slot_t rx[UCN_V6_CONFIG_TRANSFER_RX_SLOTS];
    ucn_v6_transfer_recent_slot_t recent[UCN_V6_CONFIG_TRANSFER_RECENT];
    ucn_v6_transfer_credit_slot_t
        credits[UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS * 2U];
    ucn_v6_transfer_credit_reservation_slot_t
        reservations[UCN_V6_CONFIG_TRANSFER_CREDIT_RESERVATIONS];
    ucn_v6_transfer_stats_t stats;
    bool selected;
    uint16_t selected_tx_index;
    uint16_t selected_fragment_index;
    bool initialized;
    bool faulted;
    uint64_t canary;
};

#endif
