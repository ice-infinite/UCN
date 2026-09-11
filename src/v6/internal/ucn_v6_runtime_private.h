#ifndef UCN_V6_RUNTIME_PRIVATE_H
#define UCN_V6_RUNTIME_PRIVATE_H

#include "ucn/v6/ucn_v6_runtime.h"

#define UCN_V6_RUNTIME_MAGIC UINT32_C(0x5636524E)
#define UCN_V6_RUNTIME_CANARY UINT64_C(0x52554E54494D4536)

typedef enum ucn_v6_runtime_invalidation_source {
    UCN_V6_RUNTIME_INVALIDATION_NONE = 0,
    UCN_V6_RUNTIME_INVALIDATION_ADAPTER = 1,
    UCN_V6_RUNTIME_INVALIDATION_SECURITY = 2,
    UCN_V6_RUNTIME_INVALIDATION_CAPABILITY = 3
} ucn_v6_runtime_invalidation_source_t;

typedef struct ucn_v6_runtime_release_slot {
    bool occupied;
    uint64_t buffer_token;
    ucn_v6_result_t result;
    ucn_v6_driver_timestamp_t timestamp;
} ucn_v6_runtime_release_slot_t;

#define UCN_V6_RUNTIME_TX_SLOTS                                           \
    ((size_t)(UCN_V6_CONFIG_QOS_Q0_DEPTH + UCN_V6_CONFIG_QOS_Q1_DEPTH +  \
              UCN_V6_CONFIG_QOS_Q2_DEPTH + UCN_V6_CONFIG_QOS_Q3_DEPTH))

typedef enum ucn_v6_runtime_tx_security_kind {
    UCN_V6_RUNTIME_TX_ENDPOINT = 0,
    UCN_V6_RUNTIME_TX_PEER_DISCOVERY = 1
} ucn_v6_runtime_tx_security_kind_t;

typedef struct ucn_v6_runtime_tx_slot {
    bool occupied;
    bool release_to_app;
    bool relay;
    ucn_v6_runtime_tx_security_kind_t security_kind;
    uint64_t buffer_token;
    union {
        struct {
            ucn_v6_frame_t frame;
            ucn_v6_route_select_request_t route_request;
            bool exact_route_ref;
            ucn_v6_route_path_ref_t route_ref;
            bool direct_peer_discovery;
            ucn_v6_session_key_t direct_peer_session;
            uint16_t direct_link_id;
            uint32_t direct_link_generation;
        } local;
        ucn_v6_security_open_result_t relay_opened;
    } semantic;
    bool request_timestamp;
    bool transfer_fragment;
    uint64_t transfer_message_id;
    uint16_t transfer_fragment_index;
    uint16_t payload_length;
    uint8_t payload[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
} ucn_v6_runtime_tx_slot_t;

typedef struct ucn_v6_runtime_qos_inflight {
    uint64_t buffer_token;
    bool release_to_app;
    bool bootstrap;
    ucn_v6_driver_event_key_t adapter_key;
    ucn_v6_session_key_t next_hop_parent;
    ucn_v6_session_key_t endpoint_parent;
} ucn_v6_runtime_qos_inflight_t;

#define UCN_V6_RUNTIME_BOOTSTRAP_TX_SLOTS \
    ((size_t)(UCN_V6_CONFIG_BOOTSTRAP_PENDING * 2U + 1U))

typedef struct ucn_v6_runtime_bootstrap_tx_slot {
    bool occupied;
    uint64_t buffer_token;
    uint16_t link_id;
    uint32_t link_generation;
    uint16_t protocol_opcode;
    uint16_t frame_length;
    uint64_t deadline_us;
    uint8_t frame[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
} ucn_v6_runtime_bootstrap_tx_slot_t;

typedef struct ucn_v6_runtime_bootstrap_initiation {
    bool occupied;
    uint16_t link_id;
    uint32_t link_generation;
    uint32_t local_peer_discriminator;
    ucn_v6_address_class_t address_class;
    uint32_t realm_id;
    ucn_v6_bootstrap_hello_t hello;
    uint64_t deadline_us;
} ucn_v6_runtime_bootstrap_initiation_t;

typedef struct ucn_v6_runtime_transfer_slot {
    bool occupied;
    bool fragment_queued;
    uint64_t message_id;
    uint64_t buffer_token;
    ucn_v6_route_path_ref_t route_ref;
    ucn_v6_message_descriptor_t message;
    ucn_v6_message_class_t message_class;
    bool has_hop_budget;
    uint64_t initial_hop_budget_us;
    uint64_t remaining_hop_budget_us;
    uint8_t local_priority;
} ucn_v6_runtime_transfer_slot_t;

typedef struct ucn_v6_runtime_transfer_delivery {
    bool pending;
    ucn_v6_session_key_t origin;
    uint64_t operation_id;
    uint64_t message_id;
} ucn_v6_runtime_transfer_delivery_t;

typedef enum ucn_v6_runtime_rx_phase {
    UCN_V6_RUNTIME_RX_IDLE = 0,
    UCN_V6_RUNTIME_RX_RAW = 1,
    UCN_V6_RUNTIME_RX_OPENED = 2,
    UCN_V6_RUNTIME_RX_RELAY = 3,
    UCN_V6_RUNTIME_RX_COMPLETE = 4,
    UCN_V6_RUNTIME_RX_TRANSFER_DELIVERY = 5,
    UCN_V6_RUNTIME_RX_TRANSFER_RESULT = 6
} ucn_v6_runtime_rx_phase_t;

#if UCN_V6_FEATURE_REALTIME_ENABLED
typedef enum ucn_v6_runtime_time_role {
    UCN_V6_RUNTIME_TIME_NONE = 0,
    UCN_V6_RUNTIME_TIME_MEMBER = 1,
    UCN_V6_RUNTIME_TIME_MASTER = 2
} ucn_v6_runtime_time_role_t;

typedef struct ucn_v6_runtime_time_slot {
    bool occupied;
    bool tx_queued;
    bool tx_bound;
    bool local_tx_complete;
    bool response_semantic_frozen;
    bool response_sent;
    ucn_v6_runtime_time_role_t role;
    uint64_t handle_cookie;
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    uint32_t sync_sequence;
    uint64_t deadline_us;
    ucn_v6_route_path_ref_t route_ref;
    ucn_v6_route_path_ref_t inbound_forward_route_ref;
    ucn_v6_principal_t remote_principal;
    ucn_v6_binding_key_t local_binding;
    ucn_v6_binding_key_t remote_binding;
    uint32_t session_generation;
    ucn_v6_time_local_capture_t local_rx;
    uint64_t tx_buffer_token;
    ucn_v6_driver_event_key_t tx_key;
    ucn_v6_time_local_capture_t local_tx;
    ucn_v6_time_sync_response_t frozen_response;
} ucn_v6_runtime_time_slot_t;
#endif

#define UCN_V6_RUNTIME_RELEASE_SLOTS                                      \
    ((size_t)(UCN_V6_CONFIG_ADAPTER_TX_SLOTS +                            \
              UCN_V6_CONFIG_TRANSFER_TX_SLOTS +                          \
              UCN_V6_CONFIG_QOS_Q0_DEPTH + UCN_V6_CONFIG_QOS_Q1_DEPTH +  \
              UCN_V6_CONFIG_QOS_Q2_DEPTH + UCN_V6_CONFIG_QOS_Q3_DEPTH +  \
              UCN_V6_CONFIG_QOS_INFLIGHT))

struct ucn_v6_runtime_owner {
    uint32_t magic;
    uint16_t schema;
    uint64_t layout_hash;
    ucn_v6_runtime_config_t config;
    uint8_t rx_frame[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
    uint8_t rx_plaintext[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
    ucn_v6_security_open_result_t opened_rx;
    ucn_v6_runtime_rx_phase_t rx_phase;
    ucn_v6_runtime_ingress_disposition_t rx_disposition;
    bool callback_active;
    bool ingress_active;
    ucn_v6_driver_rx_view_t active_rx;
    ucn_v6_runtime_release_slot_t releases[UCN_V6_RUNTIME_RELEASE_SLOTS];
    ucn_v6_runtime_tx_slot_t tx_slots[UCN_V6_RUNTIME_TX_SLOTS];
    ucn_v6_runtime_qos_inflight_t
        qos_inflight[UCN_V6_CONFIG_QOS_INFLIGHT];
    ucn_v6_runtime_bootstrap_tx_slot_t
        bootstrap_tx[UCN_V6_RUNTIME_BOOTSTRAP_TX_SLOTS];
    ucn_v6_runtime_bootstrap_initiation_t
        bootstrap_initiations[UCN_V6_CONFIG_BOOTSTRAP_PENDING * 2U];
    ucn_v6_bootstrap_reassembly_t
        bootstrap_reassembly[UCN_V6_CONFIG_BOOTSTRAP_PENDING * 2U];
    ucn_v6_runtime_bootstrap_ingress_t bootstrap_ingress_work;
    ucn_v6_runtime_bootstrap_action_t bootstrap_action_work;
    ucn_v6_join_commit_t bootstrap_commit_work;
    bool bootstrap_tx_turn;
    ucn_v6_runtime_transfer_slot_t
        transfer_slots[UCN_V6_CONFIG_TRANSFER_TX_SLOTS];
    ucn_v6_runtime_transfer_delivery_t transfer_delivery;
    ucn_v6_transfer_result_t transfer_result;
    uint8_t transfer_cursor;
    uint64_t next_internal_buffer_token;
    uint8_t tx_payload_work[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
    uint8_t tx_frame_work[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
    uint8_t tx_encoded_work[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
    ucn_v6_stack_invalidation_t pending_invalidation;
    ucn_v6_runtime_invalidation_source_t pending_source;
    bool invalidation_fanout_complete;
    uint8_t timer_cursor;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    ucn_v6_runtime_time_slot_t
        time_slots[UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGES];
    uint64_t next_time_handle_cookie;
#endif
    ucn_v6_runtime_view_t stats;
    bool initialized;
    uint64_t canary;
};

#endif
