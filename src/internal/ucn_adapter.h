#ifndef UCN_ADAPTER_H
#define UCN_ADAPTER_H

#include "internal/ucn_owner.h"
#include "ucn/ucn_product.h"

#define UCN_I_ADAPTER_SCHEMA UINT16_C(1)

typedef uint8_t ucn_i_adapter_tx_state_t;
enum {
    UCN_I_ADAPTER_TX_FREE = 0,
    UCN_I_ADAPTER_TX_RESERVED = 1,
    UCN_I_ADAPTER_TX_SUBMITTING = 2,
    UCN_I_ADAPTER_TX_SUBMITTED = 3,
    UCN_I_ADAPTER_TX_COMPLETED = 4,
    UCN_I_ADAPTER_TX_NOT_SUBMITTED = 5,
    UCN_I_ADAPTER_TX_IN_DOUBT = 6,
    UCN_I_ADAPTER_TX_CANCELLED = 7
};

typedef struct ucn_i_link {
    void *driver_context;
    ucn_tx_port_vtable_t tx;
    uint32_t instance_generation;
    uint16_t handle_generation;
    uint16_t frame_mtu;
    uint8_t valid;
    uint8_t up;
    uint8_t fenced;
    uint8_t reserved_zero;
} ucn_i_link_t;

typedef struct ucn_i_adapter_tx_token {
    ucn_result_t terminal_result;
    uint32_t link_instance_generation;
    uint16_t generation;
    uint16_t link_slot;
    uint16_t core_tx_slot;
    uint8_t state;
    uint8_t terminal_latched;
    uint8_t completion_consumed;
    uint8_t reserved_zero;
} ucn_i_adapter_tx_token_t;

typedef struct ucn_i_adapter_rx_slot {
    ucn_rx_meta_t meta;
    uint32_t link_instance_generation;
    uint16_t generation;
    uint16_t link_slot;
    uint16_t frame_bytes;
    uint8_t state;
    uint8_t reserved_zero;
    uint8_t frame[UCN_ADAPTER_FRAME_BYTES];
} ucn_i_adapter_rx_slot_t;

typedef struct ucn_i_adapter {
    uint32_t magic;
    uint32_t runtime_instance;
    uint16_t owner_instance;
    uint16_t schema;
    uint16_t link_count;
    uint16_t tx_allocate_cursor;
    uint16_t rx_allocate_cursor;
    uint16_t rx_claim_cursor;
    uint8_t faulted;
    uint8_t rx_enabled;
    ucn_i_lock_ops_t lock;
    ucn_i_lock_ops_t driver_callback_gate;
    ucn_i_owner_mailbox_t mailbox;
    ucn_i_link_t links[UCN_LINK_COUNT];
    ucn_i_adapter_tx_token_t tx_tokens[UCN_ADAPTER_TX_SLOT_COUNT];
    ucn_i_adapter_rx_slot_t rx_slots[UCN_ADAPTER_RX_SLOT_COUNT];
} ucn_i_adapter_t;

typedef struct ucn_i_adapter_tx_view {
    ucn_result_t terminal_result;
    uint16_t core_tx_slot;
    uint8_t state;
    uint8_t terminal_latched;
} ucn_i_adapter_tx_view_t;

typedef struct ucn_i_adapter_rx_view {
    const uint8_t *frame;
    size_t frame_bytes;
    ucn_rx_meta_t meta;
    ucn_driver_token_t token;
    uint32_t link_instance_generation;
    uint16_t link_slot;
} ucn_i_adapter_rx_view_t;

ucn_result_t ucn_i_adapter_init(ucn_i_adapter_t *adapter,
                                uint32_t runtime_instance,
                                uint16_t owner_instance,
                                const ucn_ports_t *ports);
ucn_result_t ucn_i_adapter_link_handle(const ucn_i_adapter_t *adapter,
                                       uint16_t link_index,
                                       ucn_link_handle_t *handle_out);
ucn_result_t ucn_i_adapter_link_snapshot(const ucn_i_adapter_t *adapter,
                                         uint16_t link_index,
                                         uint32_t *instance_out,
                                         uint16_t *mtu_out,
                                         bool *ready_out);
ucn_result_t ucn_i_adapter_set_rx_enabled(ucn_i_adapter_t *adapter,
                                          bool enabled);
ucn_result_t ucn_i_adapter_tx_reserve(ucn_i_adapter_t *adapter,
                                      uint16_t link_index,
                                      uint32_t link_instance_generation,
                                      uint16_t core_tx_slot,
                                      ucn_driver_token_t *token_out);
ucn_result_t ucn_i_adapter_tx_submit(ucn_i_adapter_t *adapter,
                                     ucn_driver_token_t token,
                                     const uint8_t *frame,
                                     size_t frame_bytes,
                                     ucn_i_adapter_tx_view_t *view_out);
ucn_result_t ucn_i_adapter_tx_complete(ucn_i_adapter_t *adapter,
                                       ucn_driver_token_t token,
                                       ucn_result_t result,
                                       const ucn_tx_meta_t *meta);
ucn_result_t ucn_i_adapter_tx_view(ucn_i_adapter_t *adapter,
                                   ucn_driver_token_t token,
                                   ucn_i_adapter_tx_view_t *view_out);
ucn_result_t ucn_i_adapter_tx_cancel(ucn_i_adapter_t *adapter,
                                     ucn_driver_token_t token);
ucn_result_t ucn_i_adapter_tx_retire(ucn_i_adapter_t *adapter,
                                     ucn_driver_token_t token);
ucn_result_t ucn_i_adapter_rx_publish(ucn_i_adapter_t *adapter,
                                      ucn_link_handle_t link,
                                      const uint8_t *frame,
                                      size_t frame_bytes,
                                      const ucn_rx_meta_t *meta);
ucn_result_t ucn_i_adapter_rx_claim(ucn_i_adapter_t *adapter,
                                    ucn_i_adapter_rx_view_t *view_out);
ucn_result_t ucn_i_adapter_rx_retire(ucn_i_adapter_t *adapter,
                                     ucn_driver_token_t token);
ucn_result_t ucn_i_adapter_link_event(ucn_i_adapter_t *adapter,
                                      ucn_link_handle_t link,
                                      ucn_driver_link_event_t event,
                                      const ucn_link_event_meta_t *meta);
bool ucn_i_adapter_has_work(ucn_i_adapter_t *adapter);
bool ucn_i_adapter_has_runnable_work(ucn_i_adapter_t *adapter);
ucn_result_t ucn_i_adapter_faulted(ucn_i_adapter_t *adapter,
                                   bool *faulted_out);
ucn_result_t ucn_i_adapter_destroy(ucn_i_adapter_t *adapter);

#endif
