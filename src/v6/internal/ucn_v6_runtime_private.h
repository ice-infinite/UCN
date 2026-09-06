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

#if UCN_V6_FEATURE_REALTIME_ENABLED
typedef enum ucn_v6_runtime_time_role {
    UCN_V6_RUNTIME_TIME_NONE = 0,
    UCN_V6_RUNTIME_TIME_MEMBER = 1,
    UCN_V6_RUNTIME_TIME_MASTER = 2
} ucn_v6_runtime_time_role_t;

typedef struct ucn_v6_runtime_time_slot {
    bool occupied;
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
    bool callback_active;
    bool ingress_active;
    ucn_v6_driver_rx_view_t active_rx;
    ucn_v6_runtime_release_slot_t releases[UCN_V6_RUNTIME_RELEASE_SLOTS];
    ucn_v6_stack_invalidation_t pending_invalidation;
    ucn_v6_runtime_invalidation_source_t pending_source;
    bool invalidation_fanout_complete;
    uint8_t timer_cursor;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    ucn_v6_runtime_time_slot_t
        time_slots[UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGES];
    uint64_t next_time_handle_cookie;
    bool time_tx_active;
    uint8_t time_payload_work[UCN_V6_TIME_SYNC_RESPONSE_BYTES];
    uint8_t time_frame_work[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
    uint8_t time_encoded[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
#endif
    ucn_v6_runtime_view_t stats;
    bool initialized;
    uint64_t canary;
};

#endif
