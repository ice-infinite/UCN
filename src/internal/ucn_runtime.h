#ifndef UCN_RUNTIME_H
#define UCN_RUNTIME_H

#include "internal/ucn_adapter.h"
#include "internal/ucn_owner.h"
#include "internal/ucn_wire.h"

#define UCN_I_NODE_SCHEMA UINT16_C(1)
#define UCN_I_CORE_OWNER_INSTANCE UINT16_C(1)
#define UCN_I_ADAPTER_OWNER_INSTANCE UINT16_C(2)
#define UCN_I_NO_SLOT UINT16_MAX

typedef struct ucn_i_binding {
    uint64_t replay_bitmap;
    uint64_t principal_digest;
    uint32_t address;
    uint32_t binding_generation;
    uint32_t replay_highest_sequence;
} ucn_i_binding_t;

typedef struct ucn_i_endpoint {
    ucn_endpoint_receive_fn receive;
    void *context;
    uint16_t service_id;
    uint16_t generation;
    uint8_t valid;
    uint8_t callback_active;
    uint8_t reserved_zero[2];
} ucn_i_endpoint_t;

typedef struct ucn_i_static_path {
    uint32_t destination_address;
    uint32_t destination_binding_generation;
    uint32_t link_instance_generation;
    uint16_t link_index;
    uint16_t path_frame_mtu;
    uint16_t generation;
    uint16_t references;
    uint8_t valid;
    uint8_t reserved_zero[3];
} ucn_i_static_path_t;

typedef uint8_t ucn_i_tx_state_t;
enum {
    UCN_I_TX_FREE = 0,
    UCN_I_TX_QUEUED = 1,
    UCN_I_TX_SUBMITTED = 2
};

typedef struct ucn_i_tx_slot {
    uint64_t absolute_deadline_us;
    ucn_driver_token_t adapter_token;
    uint32_t destination_address;
    uint32_t destination_binding_generation;
    uint32_t origin_sequence;
    uint16_t service_id;
    uint16_t payload_bytes;
    uint16_t frame_bytes;
    uint16_t path_slot;
    uint16_t request_slot;
    uint8_t traffic_class;
    uint8_t hop_limit;
    uint8_t state;
    uint8_t encoded;
    uint8_t tracked;
    uint8_t cancel_required;
    uint8_t cancel_requested;
    uint8_t timeout_requested;
    uint8_t reserved_zero[2];
    uint8_t frame[UCN_ADAPTER_FRAME_BYTES];
} ucn_i_tx_slot_t;

typedef struct ucn_i_receipt {
    ucn_send_view_t view;
    uint16_t generation;
    uint8_t valid;
    uint8_t reserved_zero;
} ucn_i_receipt_t;

typedef struct ucn_i_attempt {
    ucn_driver_token_t adapter_token;
    uint16_t generation;
    uint16_t tx_slot;
    uint16_t path_slot;
    uint8_t valid;
    uint8_t reserved_zero;
} ucn_i_attempt_t;

typedef struct ucn_i_buffer_obligation {
    uint16_t generation;
    uint16_t tx_slot;
    uint8_t valid;
    uint8_t released;
    uint8_t reserved_zero[2];
} ucn_i_buffer_obligation_t;

typedef struct ucn_i_request {
    ucn_send_completion_fn completion;
    void *completion_context;
    uint16_t generation;
    uint16_t tx_slot;
    uint16_t receipt_slot;
    uint16_t attempt_slot;
    uint16_t buffer_slot;
    uint8_t valid;
    uint8_t terminal;
} ucn_i_request_t;

typedef struct ucn_i_callback_send_snapshot {
    ucn_send_view_t view;
    uint16_t request_generation;
    uint8_t valid;
    uint8_t reserved_zero;
} ucn_i_callback_send_snapshot_t;

typedef struct ucn_i_callback_read_snapshot {
    ucn_stats_t stats;
    ucn_i_callback_send_snapshot_t sends[UCN_REQUEST_COUNT];
    ucn_link_handle_t links[UCN_LINK_COUNT];
    ucn_callback_scope_t scope;
    uint16_t link_count;
    uint8_t active;
    uint8_t reserved_zero;
} ucn_i_callback_read_snapshot_t;

struct ucn_node {
    uint32_t magic;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint32_t local_address;
    uint32_t local_binding_generation;
    uint32_t last_origin_sequence;
    uint64_t local_principal_digest;
    uint64_t compiled_manifest_hash;
    uint64_t last_now_us;
    uint64_t callback_read_nonce;
    uint16_t schema;
    uint16_t storage_layout;
    uint16_t owner_instance;
    uint16_t binding_count;
    uint16_t link_count;
    uint16_t endpoint_allocate_cursor;
    uint16_t path_allocate_cursor;
    uint16_t request_allocate_cursor;
    uint16_t receipt_allocate_cursor;
    uint16_t attempt_allocate_cursor;
    uint16_t buffer_allocate_cursor;
    uint16_t completion_cursor;
    uint16_t timer_cursor;
    uint8_t address_width;
    uint8_t lifecycle;
    uint8_t trusted_o0_network;
    uint8_t faulted;
    uint8_t time_initialized;
    uint8_t work_cursor;
    uint8_t scheduler_cursor;
    uint8_t queue_cursor[UCN_TRAFFIC_CLASS_COUNT];
    uint8_t queue_allocate_cursor[UCN_TRAFFIC_CLASS_COUNT];
    ucn_i_lock_ops_t lock;
    ucn_i_callback_gate_t callback_gate;
    ucn_i_callback_read_snapshot_t callback_read_snapshot;
    ucn_i_binding_t bindings[UCN_BINDING_COUNT];
    ucn_i_endpoint_t endpoints[UCN_ENDPOINT_COUNT];
    ucn_i_static_path_t paths[UCN_STATIC_PATH_COUNT];
    ucn_i_request_t requests[UCN_REQUEST_COUNT];
    ucn_i_receipt_t receipts[UCN_RECEIPT_COUNT];
    ucn_i_attempt_t attempts[UCN_ATTEMPT_COUNT];
    ucn_i_buffer_obligation_t buffers[UCN_BUFFER_OBLIGATION_COUNT];
    ucn_i_tx_slot_t tx_slots[UCN_TX_SLOT_COUNT];
    ucn_i_adapter_t adapter;
    ucn_stats_t stats;
};

#endif
