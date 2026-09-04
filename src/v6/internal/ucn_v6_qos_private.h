#ifndef UCN_V6_QOS_PRIVATE_H
#define UCN_V6_QOS_PRIVATE_H

#include "ucn/v6/ucn_v6_qos.h"

#define UCN_V6_METRIC_OWNER_MAGIC UINT32_C(0x564D4554)
#define UCN_V6_QOS_OWNER_MAGIC UINT32_C(0x56514F53)
#define UCN_V6_METRIC_OWNER_CANARY UINT64_C(0x8D4F29A1C763B50E)
#define UCN_V6_QOS_OWNER_CANARY UINT64_C(0xC45E7B9021AD683F)
#define UCN_V6_QOS_QUEUE_CAPACITY                                        \
    ((size_t)(UCN_V6_CONFIG_QOS_Q0_DEPTH + UCN_V6_CONFIG_QOS_Q1_DEPTH + \
              UCN_V6_CONFIG_QOS_Q2_DEPTH + UCN_V6_CONFIG_QOS_Q3_DEPTH))

typedef struct ucn_v6_metric_slot {
    bool occupied;
    ucn_v6_metric_key_t key;
    ucn_v6_metric_sample_t last_input;
    ucn_v6_metric_sample_t filtered;
} ucn_v6_metric_slot_t;

struct ucn_v6_metric_owner {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    ucn_v6_metric_policy_t policy;
    ucn_v6_metric_slot_t slots[UCN_V6_CONFIG_METRIC_PATHS];
    ucn_v6_metric_view_t stats;
    bool initialized;
    bool faulted;
    uint64_t canary;
};

typedef struct ucn_v6_qos_queue_item {
    bool occupied;
    uint64_t buffer_token;
    uint64_t flow_id;
    ucn_v6_session_key_t source;
    ucn_v6_traffic_class_t traffic_class;
    ucn_v6_delivery_guarantee_t delivery_guarantee;
    uint16_t payload_bytes;
    uint8_t local_priority;
    bool has_hop_budget;
    uint64_t initial_budget_us;
    uint64_t remaining_budget_us;
    uint64_t enqueued_at_us;
    uint64_t arrival_order;
} ucn_v6_qos_queue_item_t;

typedef struct ucn_v6_qos_flow_state {
    bool occupied;
    ucn_v6_session_key_t source;
    uint64_t flow_id;
    uint16_t tokens[4];
    uint32_t deficit[4];
    uint64_t last_refill_us;
} ucn_v6_qos_flow_state_t;

typedef struct ucn_v6_qos_inflight {
    bool occupied;
    uint64_t buffer_token;
    uint64_t flow_id;
    ucn_v6_session_key_t source;
    ucn_v6_traffic_class_t traffic_class;
    ucn_v6_qos_completion_stage_t stage;
} ucn_v6_qos_inflight_t;

struct ucn_v6_qos_owner {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    ucn_v6_qos_policy_t policy;
    ucn_v6_qos_queue_item_t queue[UCN_V6_QOS_QUEUE_CAPACITY];
    ucn_v6_qos_flow_state_t flows[UCN_V6_CONFIG_QOS_FLOW_SLOTS];
    ucn_v6_qos_inflight_t inflight[UCN_V6_CONFIG_QOS_INFLIGHT];
    ucn_v6_qos_stats_t stats;
    uint64_t next_arrival_order;
    uint8_t schedule_cursor;
    uint8_t flow_cursor[4];
    bool selected;
    uint16_t selected_queue_index;
    ucn_v6_qos_selection_action_t selected_action;
    bool initialized;
    bool faulted;
    uint64_t canary;
};

#endif
