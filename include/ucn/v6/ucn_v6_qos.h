#ifndef UCN_V6_QOS_H
#define UCN_V6_QOS_H

#include "ucn/v6/ucn_v6_message.h"
#include "ucn/v6/ucn_v6_route.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_METRIC_ALGORITHM_DEFAULT UINT32_C(0x00060001)
#define UCN_V6_METRIC_ALL_KNOWN UINT16_C(0x00FF)

enum {
    UCN_V6_METRIC_ADMIN_COST_KNOWN = 1U << 0,
    UCN_V6_METRIC_LATENCY_KNOWN = 1U << 1,
    UCN_V6_METRIC_JITTER_KNOWN = 1U << 2,
    UCN_V6_METRIC_LOSS_KNOWN = 1U << 3,
    UCN_V6_METRIC_BITRATE_KNOWN = 1U << 4,
    UCN_V6_METRIC_QUEUE_KNOWN = 1U << 5,
    UCN_V6_METRIC_ENERGY_KNOWN = 1U << 6,
    UCN_V6_METRIC_STABILITY_KNOWN = 1U << 7
};

typedef struct ucn_v6_metric_sample {
    uint16_t known_mask;
    uint16_t administrative_cost;
    uint32_t latency_us;
    uint32_t jitter_us;
    uint32_t loss_ppm;
    uint32_t available_bitrate_bps;
    uint16_t queue_occupancy_permille;
    uint16_t energy_cost;
    uint16_t stability_score_permille;
    uint64_t measured_at_us;
    uint64_t sample_window_us;
} ucn_v6_metric_sample_t;

typedef struct ucn_v6_metric_weights {
    uint16_t administrative;
    uint16_t latency;
    uint16_t jitter;
    uint16_t loss;
    uint16_t inverse_bitrate;
    uint16_t queue;
    uint16_t energy;
    uint16_t instability;
} ucn_v6_metric_weights_t;

typedef struct ucn_v6_metric_policy {
    uint32_t algorithm_id;
    uint16_t ewma_alpha_permille;
    uint64_t stale_after_us;
    ucn_v6_metric_weights_t class_weights[4];
} ucn_v6_metric_policy_t;

typedef struct ucn_v6_metric_key {
    ucn_v6_route_domain_t domain;
    uint32_t route_generation;
    uint16_t path_id;
    uint32_t path_generation;
} ucn_v6_metric_key_t;

typedef struct ucn_v6_metric_cost {
    uint32_t algorithm_id;
    uint64_t total_cost;
    uint16_t hop_count;
} ucn_v6_metric_cost_t;

typedef struct ucn_v6_metric_view {
    uint16_t active_paths;
    uint32_t samples_ingested;
    uint32_t stale_reads;
    uint32_t rejected_unknown;
    bool faulted;
} ucn_v6_metric_view_t;

typedef struct ucn_v6_metric_owner ucn_v6_metric_owner_t;
#ifndef UCN_V6_METRIC_OWNER_STORAGE_BYTES
#define UCN_V6_METRIC_OWNER_STORAGE_BYTES                                 \
    ((size_t)(1024U + UCN_V6_CONFIG_METRIC_PATHS * 256U))
#endif
typedef union ucn_v6_metric_owner_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_METRIC_OWNER_STORAGE_BYTES];
} ucn_v6_metric_owner_storage_t;

typedef struct ucn_v6_qos_policy {
    uint16_t source_flow_burst[4];
    uint16_t source_flow_refill[4];
    uint64_t refill_period_us;
    uint64_t max_hop_budget_us[4];
    uint16_t max_flows_per_session;
    uint16_t q2_quantum_bytes;
    uint16_t q3_quantum_bytes;
} ucn_v6_qos_policy_t;

typedef struct ucn_v6_qos_enqueue_result {
    bool accepted;
    bool replaced_latest;
    uint64_t replaced_buffer_token;
    uint64_t flow_id;
} ucn_v6_qos_enqueue_result_t;

typedef enum ucn_v6_qos_selection_action {
    UCN_V6_QOS_ACTION_SEND = 1,
    UCN_V6_QOS_ACTION_DROP_EXPIRED = 2
} ucn_v6_qos_selection_action_t;

typedef struct ucn_v6_qos_selection {
    ucn_v6_qos_selection_action_t action;
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
} ucn_v6_qos_selection_t;

typedef enum ucn_v6_qos_selection_result {
    UCN_V6_QOS_SELECTION_RETRY = 1,
    UCN_V6_QOS_SELECTION_LINK_SUBMITTED = 2,
    UCN_V6_QOS_SELECTION_DROP_RETIRED = 3
} ucn_v6_qos_selection_result_t;

typedef enum ucn_v6_qos_completion_stage {
    UCN_V6_QOS_COMPLETION_LINK_SUBMITTED = 1,
    UCN_V6_QOS_COMPLETION_PHYSICAL_COMPLETED = 2,
    UCN_V6_QOS_COMPLETION_REMOTE_ACKED = 3,
    UCN_V6_QOS_COMPLETION_APPLICATION_RESULT = 4
} ucn_v6_qos_completion_stage_t;

typedef struct ucn_v6_qos_stats {
    uint32_t enqueued[4];
    uint32_t latest_replaced[4];
    uint32_t scheduler_selected[4];
    uint32_t link_submitted[4];
    uint32_t physical_completed[4];
    uint32_t remote_acked[4];
    uint32_t application_result[4];
    uint32_t dropped_expired[4];
    uint32_t rejected_quota[4];
    uint32_t reclaimed_idle_flows;
    uint16_t queued[4];
    uint16_t flow_slots;
    uint16_t inflight;
    bool selection_pending;
    bool faulted;
} ucn_v6_qos_stats_t;

typedef struct ucn_v6_qos_owner ucn_v6_qos_owner_t;
#ifndef UCN_V6_QOS_OWNER_STORAGE_BYTES
#define UCN_V6_QOS_OWNER_STORAGE_BYTES                                    \
    ((size_t)(2048U +                                                     \
              (UCN_V6_CONFIG_QOS_Q0_DEPTH + UCN_V6_CONFIG_QOS_Q1_DEPTH + \
               UCN_V6_CONFIG_QOS_Q2_DEPTH + UCN_V6_CONFIG_QOS_Q3_DEPTH) * \
                  256U +                                                   \
              UCN_V6_CONFIG_QOS_FLOW_SLOTS * 256U +                       \
              UCN_V6_CONFIG_QOS_INFLIGHT * 64U))
#endif
typedef union ucn_v6_qos_owner_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_QOS_OWNER_STORAGE_BYTES];
} ucn_v6_qos_owner_storage_t;

/* EN: Returns the frozen default metric and QoS policies.
 * 中文：返回冻结的默认 Metric 与 QoS 策略。 */
void ucn_v6_metric_default_policy(ucn_v6_metric_policy_t *policy);
void ucn_v6_qos_default_policy(ucn_v6_qos_policy_t *policy);

ucn_v6_result_t ucn_v6_metric_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_metric_policy_t *policy,
    ucn_v6_metric_owner_t **owner);
ucn_v6_result_t ucn_v6_metric_ingest(
    ucn_v6_metric_owner_t *owner,
    const ucn_v6_metric_key_t *key,
    const ucn_v6_metric_sample_t *sample);
ucn_v6_result_t ucn_v6_metric_score(
    ucn_v6_metric_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_metric_key_t *key,
    ucn_v6_traffic_class_t traffic_class,
    ucn_v6_metric_cost_t *cost);
ucn_v6_result_t ucn_v6_metric_cost_accumulate(
    const ucn_v6_metric_cost_t *prefix,
    const ucn_v6_metric_cost_t *hop,
    ucn_v6_metric_cost_t *total);
ucn_v6_result_t ucn_v6_metric_expire(
    ucn_v6_metric_owner_t *owner,
    uint64_t now_us);
ucn_v6_result_t ucn_v6_metric_copy_view(
    const ucn_v6_metric_owner_t *owner,
    ucn_v6_metric_view_t *view);

ucn_v6_result_t ucn_v6_qos_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_qos_policy_t *policy,
    ucn_v6_qos_owner_t **owner);
/* EN: Derives one canonical flow key from authenticated Wire identity.
 * 中文：从已认证 Wire 身份导出唯一 Flow Key。 */
ucn_v6_result_t ucn_v6_qos_flow_id(
    const ucn_v6_security_open_result_t *opened,
    uint64_t *flow_id);
/* EN: Enqueues a caller-owned buffer token after fixed quota admission.
 * 中文：通过固定配额准入后排入调用方持有的 Buffer Token。 */
ucn_v6_result_t ucn_v6_qos_enqueue(
    ucn_v6_qos_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_security_open_result_t *opened,
    uint64_t buffer_token,
    uint16_t payload_bytes,
    uint8_t local_priority,
    ucn_v6_qos_enqueue_result_t *result);
/* EN: Peeks one bounded-fair decision; ownership changes only on completion.
 * 中文：预取一次有界公平决策；仅完成确认后转移所有权。 */
ucn_v6_result_t ucn_v6_qos_select_next(
    ucn_v6_qos_owner_t *owner,
    uint64_t now_us,
    ucn_v6_qos_selection_t *selection);
ucn_v6_result_t ucn_v6_qos_complete_selection(
    ucn_v6_qos_owner_t *owner,
    uint64_t buffer_token,
    ucn_v6_qos_selection_result_t result);
ucn_v6_result_t ucn_v6_qos_record_completion(
    ucn_v6_qos_owner_t *owner,
    uint64_t buffer_token,
    ucn_v6_qos_completion_stage_t stage);
ucn_v6_result_t ucn_v6_qos_retire_completion(
    ucn_v6_qos_owner_t *owner,
    uint64_t buffer_token);
/* EN: Reclaims only idle flows whose token buckets have naturally refilled to
 * their configured burst.  Reopening such a slot cannot reset admission
 * credit; active, selected, queued, inflight, or partially refilled flows are
 * never reclaimed.
 * 中文：仅回收已空闲且令牌桶已自然恢复到配置突发上限的 Flow。重新使用该槽
 * 不会重置准入额度；活动、已选择、排队、飞行中或尚未完全恢复的 Flow 永不回收。 */
ucn_v6_result_t ucn_v6_qos_reclaim_idle_flows(
    ucn_v6_qos_owner_t *owner,
    uint64_t now_us,
    uint16_t max_to_reclaim,
    uint16_t *reclaimed);
/* EN: Decrements a Hop-authenticated budget or returns DROP_EXPIRED.
 * 中文：扣减已逐跳认证预算；耗尽时返回固定 DROP_EXPIRED。 */
ucn_v6_result_t ucn_v6_qos_forward_budget(
    const ucn_v6_security_open_result_t *opened,
    uint64_t policy_max_budget_us,
    uint64_t residence_bound_us,
    uint64_t transmit_bound_us,
    ucn_v6_hop_budget_context_t *next_budget);
uint8_t ucn_v6_qos_hardware_priority(
    ucn_v6_traffic_class_t traffic_class,
    uint8_t hardware_priority_count);
/* EN: Atomically retires queued/inflight work owned by one canonical Link or
 * Session invalidation.  Capability/Path invalidations are canonical no-ops:
 * queued ingress work is re-routed at send time and is not owned by one
 * mutable route choice.  Every retired buffer token is returned to the
 * caller; insufficient output capacity causes zero writes.
 * 中文：按规范 Link 或 Session 失效事件原子回收排队/飞行中工作。Capability/
 * Path 事件是规范空操作：排队的入口工作会在发送时重新选路，并不归属于某个
 * 可变 Route 选择。全部被回收 Buffer Token 都返还调用方；输出容量不足零写入。 */
ucn_v6_result_t ucn_v6_qos_apply_invalidation(
    ucn_v6_qos_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation,
    uint64_t *retired_tokens,
    size_t retired_capacity,
    size_t *retired_count);
ucn_v6_result_t ucn_v6_qos_copy_stats(
    const ucn_v6_qos_owner_t *owner,
    ucn_v6_qos_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
