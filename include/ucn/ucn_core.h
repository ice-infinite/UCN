#ifndef UCN_CORE_H
#define UCN_CORE_H

#include "ucn/ucn_product.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t ucn_traffic_class_t;
enum {
    UCN_TRAFFIC_Q0 = 0,
    UCN_TRAFFIC_Q1 = 1,
    UCN_TRAFFIC_Q2 = 2,
    UCN_TRAFFIC_Q3 = 3,
    UCN_TRAFFIC_CLASS_COUNT = 4
};

typedef uint8_t ucn_delivery_guarantee_t;
enum {
    UCN_DELIVERY_BEST_EFFORT = 0,
    UCN_DELIVERY_LATEST = 1,
    UCN_DELIVERY_RELIABLE = 2
};

typedef uint8_t ucn_interaction_role_t;
enum {
    UCN_INTERACTION_ONE_WAY = 0,
    UCN_INTERACTION_REQUEST = 1,
    UCN_INTERACTION_RESULT = 2,
    UCN_INTERACTION_ERROR = 3
};

typedef uint8_t ucn_endpoint_disposition_t;
enum {
    UCN_ENDPOINT_ACCEPT = 1,
    UCN_ENDPOINT_DROP = 2
};

typedef struct ucn_target {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t address;
    uint32_t binding_generation;
    uint16_t service_id;
    uint16_t reserved_zero;
} ucn_target_t;

/* EN: API-scoped, callback-lifetime read capability. It is issued only
 * in an Endpoint callback and becomes invalid before that callback returns.
 * The value is process-local and must never be serialized or retained.
 * 中文：受 API 作用域约束、仅在回调生命周期内有效的只读能力。它只在 Endpoint
 * 回调中签发，并
 * 在回调返回前失效；该值仅限本进程使用，禁止序列化或留存。 */
typedef struct ucn_callback_scope {
    uint32_t runtime_instance;
    uint16_t owner_instance;
    uint16_t reserved_zero;
    uint64_t nonce;
} ucn_callback_scope_t;

typedef struct ucn_endpoint_message {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t source_address;
    uint32_t source_binding_generation;
    uint16_t service_id;
    uint8_t traffic_class;
    uint8_t hop_limit;
    const uint8_t *payload;
    size_t payload_bytes;
    uint64_t receive_timestamp_us;
    ucn_callback_scope_t callback_scope;
} ucn_endpoint_message_t;

typedef ucn_endpoint_disposition_t (*ucn_endpoint_receive_fn)(
    void *context,
    const ucn_endpoint_message_t *message);

typedef struct ucn_endpoint_config {
    uint16_t struct_size;
    uint16_t api_version;
    uint16_t service_id;
    uint16_t reserved_zero;
    ucn_endpoint_receive_fn receive;
    void *context;
} ucn_endpoint_config_t;

typedef struct ucn_static_path {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t destination_address;
    uint32_t destination_binding_generation;
    uint16_t link_index;
    uint16_t path_frame_mtu;
} ucn_static_path_t;

typedef struct ucn_send_view ucn_send_view_t;
typedef void (*ucn_send_completion_fn)(void *context,
                                       ucn_send_handle_t handle,
                                       const ucn_send_view_t *view);

typedef struct ucn_send_options {
    uint16_t struct_size;
    uint16_t api_version;
    uint64_t absolute_deadline_us;
    ucn_path_handle_t pinned_path;
    void *completion_context;
    ucn_send_completion_fn completion;
    uint8_t traffic_class;
    uint8_t delivery_guarantee;
    uint8_t interaction_role;
    uint8_t hop_limit;
    uint8_t copy_payload;
    uint8_t reserved_zero[3];
} ucn_send_options_t;

typedef uint8_t ucn_send_admission_t;
enum {
    UCN_SEND_ADMITTED = 1,
    UCN_SEND_REJECTED = 2
};

typedef uint8_t ucn_link_outcome_t;
enum {
    UCN_LINK_OUTCOME_PENDING = 1,
    UCN_LINK_OUTCOME_SUBMITTED = 2,
    UCN_LINK_OUTCOME_COMPLETE = 3,
    UCN_LINK_OUTCOME_FAILED = 4,
    UCN_LINK_OUTCOME_CANCELLED = 5,
    UCN_LINK_OUTCOME_IN_DOUBT = 6
};

struct ucn_send_view {
    uint16_t struct_size;
    uint16_t api_version;
    ucn_result_t terminal_result;
    uint32_t origin_sequence;
    uint8_t admission;
    uint8_t link_outcome;
    uint8_t buffer_released;
    uint8_t callback_delivered;
};

typedef struct ucn_step_budget {
    uint16_t struct_size;
    uint16_t api_version;
    uint16_t max_work;
    uint16_t reserved_zero;
} ucn_step_budget_t;

typedef struct ucn_step_result {
    uint16_t struct_size;
    uint16_t api_version;
    uint64_t next_deadline_us;
    uint16_t work_done;
    uint8_t more_work;
    uint8_t lifecycle;
} ucn_step_result_t;

typedef struct ucn_stats {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t tx_admitted;
    uint32_t tx_completed;
    uint32_t tx_failed;
    uint32_t rx_published;
    uint32_t rx_delivered;
    uint32_t rx_dropped;
    uint32_t malformed;
    uint32_t no_space;
} ucn_stats_t;

typedef uint8_t ucn_lifecycle_t;
enum {
    UCN_LIFECYCLE_INITIALIZED = 1,
    UCN_LIFECYCLE_STARTING = 2,
    UCN_LIFECYCLE_RUNNING = 3,
    UCN_LIFECYCLE_STOPPING = 4,
    UCN_LIFECYCLE_QUIESCENT = 5,
    UCN_LIFECYCLE_FAULT = 6
};

ucn_result_t ucn_init(void *storage,
                      size_t storage_bytes,
                      const ucn_config_t *config,
                      const ucn_ports_t *ports,
                      ucn_node_t **out_node);
ucn_result_t ucn_start(ucn_node_t *node, uint64_t now_us);
ucn_result_t ucn_step(ucn_node_t *node,
                      uint64_t now_us,
                      const ucn_step_budget_t *budget,
                      ucn_step_result_t *out_result);
ucn_result_t ucn_stop(ucn_node_t *node);
ucn_result_t ucn_deinit(ucn_node_t *node);

ucn_result_t ucn_endpoint_add(ucn_node_t *node,
                              const ucn_endpoint_config_t *config,
                              ucn_endpoint_handle_t *out_endpoint);
ucn_result_t ucn_endpoint_remove(ucn_node_t *node,
                                 ucn_endpoint_handle_t endpoint);
ucn_result_t ucn_static_path_add(ucn_node_t *node,
                                const ucn_static_path_t *path,
                                ucn_path_handle_t *out_path);
ucn_result_t ucn_static_path_remove(ucn_node_t *node,
                                   ucn_path_handle_t path);
ucn_result_t ucn_link_get(const ucn_node_t *node,
                          uint16_t link_index,
                          ucn_link_handle_t *out_link);

ucn_result_t ucn_publish(ucn_node_t *node,
                         const ucn_target_t *target,
                         const void *payload,
                         size_t payload_bytes,
                         const ucn_send_options_t *options,
                         ucn_send_handle_t *out_handle);
ucn_result_t ucn_send_query(const ucn_node_t *node,
                            ucn_send_handle_t handle,
                            ucn_send_view_t *out_view);
ucn_result_t ucn_send_cancel(ucn_node_t *node, ucn_send_handle_t handle);
ucn_result_t ucn_send_forget(ucn_node_t *node, ucn_send_handle_t handle);
ucn_result_t ucn_get_stats(const ucn_node_t *node, ucn_stats_t *out_stats);

/* EN: Read-only callback APIs. Ordinary query APIs remain gated while a
 * callback is active; only the exact capability carried by this callback's
 * message may read its immutable snapshot.
 * 中文：回调专用只读 API。回调活动期间普通查询仍被门禁拒绝；只有本次消息携带
 * 的精确 capability 才能读取其不可变快照。 */
ucn_result_t ucn_callback_link_get(const ucn_node_t *node,
                                   ucn_callback_scope_t scope,
                                   uint16_t link_index,
                                   ucn_link_handle_t *out_link);
ucn_result_t ucn_callback_send_query(const ucn_node_t *node,
                                     ucn_callback_scope_t scope,
                                     ucn_send_handle_t handle,
                                     ucn_send_view_t *out_view);
ucn_result_t ucn_callback_get_stats(const ucn_node_t *node,
                                    ucn_callback_scope_t scope,
                                    ucn_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif
