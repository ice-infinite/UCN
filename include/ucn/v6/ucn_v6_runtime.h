#ifndef UCN_V6_RUNTIME_H
#define UCN_V6_RUNTIME_H

/* Standard v6 runtime composition.
 *
 * Drivers only publish Adapter records. This owner supplies the immutable
 * Stack Owner hook graph, owns RX/TX-completion retirement, fans dependency
 * invalidations through every enabled module, and binds Realtime T2/T3 to
 * actual Adapter event keys. Product code supplies only the final authenticated
 * ingress dispatcher and buffer-return boundary; it cannot reorder phases.
 *
 * v6 标准运行时组合层。Driver 只发布 Adapter 记录；本 Owner 固定 Stack Owner
 * hook 图，持有 RX/TX completion 退休、依赖失效扇出，并把 Realtime T2/T3
 * 绑定到真实 Adapter 事件键。产品只提供最终认证消息分派和 Buffer 返还边界，
 * 无权重排核心阶段。 */

#include "ucn/v6/ucn_v6_adapter.h"
#include "ucn/v6/ucn_v6_bootstrap.h"
#include "ucn/v6/ucn_v6_qos.h"
#include "ucn/v6/ucn_v6_transfer.h"

#if UCN_V6_FEATURE_REALTIME_ENABLED
#include "ucn/v6/ucn_v6_realtime.h"
#endif
#if UCN_V6_FEATURE_CLUSTER_ENABLED
#include "ucn/v6/ucn_v6_cluster.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucn_v6_runtime_owner ucn_v6_runtime_owner_t;

typedef enum ucn_v6_runtime_ingress_disposition {
    UCN_V6_RUNTIME_INGRESS_CONSUMED = 1,
    UCN_V6_RUNTIME_INGRESS_DROP = 2,
    UCN_V6_RUNTIME_INGRESS_RETRY = 3
} ucn_v6_runtime_ingress_disposition_t;

typedef struct ucn_v6_runtime_app_ops {
    void *context;
    /* Called only from serialized RX_INGRESS. CONSUMED/DROP retires the exact
     * Adapter item; RETRY preserves it and must represent bounded backpressure.
     * 只在串行 RX_INGRESS 调用；CONSUMED/DROP 退休精确 Adapter 项，RETRY
     * 保留原项且只能表达有界背压。 */
    ucn_v6_result_t (*handle_ingress)(
        void *context, ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
        const uint8_t *encoded_frame, size_t encoded_length,
        const ucn_v6_driver_rx_view_t *rx,
        ucn_v6_runtime_ingress_disposition_t *disposition);
    /* Returns a caller buffer token after physical completion/cancellation or
     * dependency invalidation. Failure keeps it in a fixed Runtime retry slot.
     * 物理完成/取消或依赖失效后返还调用方 Buffer token；失败时保留于固定重试槽。 */
    ucn_v6_result_t (*release_buffer)(
        void *context, uint64_t buffer_token, ucn_v6_result_t result,
        const ucn_v6_driver_timestamp_t *timestamp);
    /* Optional final application dependency fence. Core owners have already
     * consumed the invalidation when this callback runs.
     * 可选的最终应用依赖 Fence；调用时所有 Core Owner 已消费同一失效事件。 */
    ucn_v6_result_t (*apply_endpoint_invalidation)(
        void *context, const ucn_v6_stack_invalidation_t *invalidation);
} ucn_v6_runtime_app_ops_t;

typedef struct ucn_v6_runtime_config {
    /* Nonzero boot/runtime incarnation used to reject handles copied from a
     * prior Runtime instance at the same address. It must not repeat while
     * any previously issued handle can still be presented.
     * 非零启动/Runtime 代际，用于拒绝同地址旧实例签发的句柄；只要旧句柄仍
     * 可能被提交，该值就不得复用。 */
    uint64_t runtime_instance_generation;
    /* Every referenced owner must already be initialized, must remain alive
     * for the complete Runtime lifetime, and must not reside in the Runtime
     * storage being initialized. Runtime borrows these objects; it never owns
     * or reconstructs them.
     * 所有 Owner 必须先完成初始化、生命周期覆盖整个 Runtime，且不得位于本次
     * Runtime Storage 内。Runtime 只借用这些对象，不拥有也不重建它们。 */
    ucn_v6_adapter_owner_t *adapter;
    ucn_v6_bootstrap_owner_t *bootstrap;
    ucn_v6_security_manager_t *security;
    ucn_v6_capability_owner_t *capability;
    ucn_v6_route_owner_t *route;
    ucn_v6_metric_owner_t *metric;
    ucn_v6_qos_owner_t *qos;
    ucn_v6_transfer_owner_t *transfer;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    ucn_v6_realtime_owner_t *realtime;
#endif
#if UCN_V6_FEATURE_CLUSTER_ENABLED
    ucn_v6_cluster_owner_t *cluster;
#endif
    ucn_v6_runtime_app_ops_t app;
} ucn_v6_runtime_config_t;

typedef union ucn_v6_runtime_owner_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_RUNTIME_OWNER_STORAGE_BYTES];
} ucn_v6_runtime_owner_storage_t;

typedef struct ucn_v6_runtime_view {
    uint32_t rx_consumed;
    uint32_t rx_dropped;
    uint32_t rx_retried;
    uint32_t tx_completions;
    uint32_t released_buffers;
    uint32_t invalidations;
    uint32_t link_reopens;
    uint32_t realtime_exchanges_started;
    uint32_t realtime_tx_timestamps_captured;
    uint32_t realtime_exchanges_completed;
    uint32_t realtime_exchanges_expired;
    bool faulted;
} ucn_v6_runtime_view_t;

/* EN: Initializes the standard fixed-capacity Runtime composition.
 * 中文：初始化标准固定容量 Runtime 组合层。 */
ucn_v6_result_t ucn_v6_runtime_init_in_place(
    void *storage, size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_runtime_config_t *config,
    ucn_v6_runtime_owner_t **runtime);

/* Produces the one canonical Stack Owner hook set. Product code must not mix
 * these hooks with a second phase implementation.
 * 生成唯一规范 Stack Owner hooks；产品不得混入第二套阶段实现。 */
ucn_v6_result_t ucn_v6_runtime_make_stack_hooks(
    ucn_v6_runtime_owner_t *runtime, ucn_v6_stack_hooks_t *hooks);

/* Quiesces one Link, retires every Adapter-owned buffer into the Runtime
 * release queue, and publishes the exact old Link generation for canonical
 * dependency fan-out. Products must use this wrapper after Runtime install.
 * 静止一个 Link，把 Adapter 持有的 Buffer 纳入 Runtime 返还队列，并发布精确
 * 旧 Link 代际执行规范失效扇出。安装 Runtime 后产品必须使用本入口 reopen。 */
ucn_v6_result_t ucn_v6_runtime_reopen_link(
    ucn_v6_runtime_owner_t *runtime, uint16_t link_id,
    uint32_t *new_link_generation);

#if UCN_V6_FEATURE_REALTIME_ENABLED
/* Opaque local capability for one Runtime-owned time exchange.  Applications
 * may copy it, but must not interpret or synthesize its words.  It becomes
 * invalid when the exchange completes/expires or Runtime is reinitialized.
 * 单个 Runtime 时间事务的不透明本地能力。应用可复制但不得解释或伪造其中
 * 字段；事务完成、超时或 Runtime 重建后立即失效。 */
typedef struct ucn_v6_runtime_time_handle {
    uint64_t opaque[2];
} ucn_v6_runtime_time_handle_t;

/* Starts the Master side by constructing, protecting and enqueueing the exact
 * TIME_SYNC frame on one immutable Route reference.  T1 is accepted only from
 * the matching Adapter TX completion.
 * 在不可变 Route 引用上由 Runtime 构造、保护并排队精确 TIME_SYNC；T1 只能
 * 来自匹配的 Adapter TX completion。 */
ucn_v6_result_t ucn_v6_runtime_time_start_sync(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_route_path_ref_t *forward_route_ref,
    const ucn_v6_time_sync_announce_t *announce,
    uint64_t buffer_token,
    uint64_t now_us,
    ucn_v6_runtime_time_handle_t *handle);

/* Must be called from handle_ingress for the exact active TIME_SYNC Adapter RX
 * item.  Runtime verifies and freezes the reverse Path, captures T2, and
 * returns the only handle that may enqueue its DELAY_REQUEST.
 * 必须在 handle_ingress 内针对精确 TIME_SYNC Adapter RX 项调用。Runtime
 * 校验并冻结反向 Path、捕获 T2，并返回唯一可排队 DELAY_REQUEST 的句柄。 */
ucn_v6_result_t ucn_v6_runtime_time_observe_sync(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_security_open_result_t *opened_sync,
    const ucn_v6_driver_rx_view_t *rx,
    const ucn_v6_route_path_ref_t *reverse_route_ref,
    uint64_t now_us,
    ucn_v6_runtime_time_handle_t *handle);

/* Consumes an exact Runtime-issued Member handle, constructs and protects the
 * matching DELAY_REQUEST, and binds T3 to the Adapter reservation it creates.
 * Caller supplies only the application buffer token returned after retirement.
 * 消费 Runtime 签发的 Member 句柄，构造并保护匹配的 DELAY_REQUEST，并把 T3
 * 绑定到自身创建的 Adapter reservation；调用方只提供最终返还的 Buffer token。 */
ucn_v6_result_t ucn_v6_runtime_time_send_delay_request(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_runtime_time_handle_t *handle,
    uint64_t buffer_token,
    uint64_t now_us);

/* Must be called from handle_ingress for an authenticated DELAY_REQUEST.
 * Runtime matches the Master transaction and actual T4 RX event, then builds,
 * protects and enqueues the exact T1/T4 DELAY_RESPONSE on the frozen forward
 * Path.  Duplicate requests replay the same semantic T1/T4 response.
 * 必须在 handle_ingress 内处理认证 DELAY_REQUEST。Runtime 匹配 Master 事务
 * 与真实 T4 RX 事件，再在冻结正向 Path 上构造、保护并排队精确 T1/T4 响应；
 * 重复请求只重发同一语义响应。 */
ucn_v6_result_t ucn_v6_runtime_time_respond_delay_request(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_security_open_result_t *opened_delay_request,
    const ucn_v6_driver_rx_view_t *rx,
    uint64_t buffer_token,
    uint64_t now_us);

/* Must be called from handle_ingress for authenticated DELAY_RESPONSE.
 * 必须在 handle_ingress 中针对认证 DELAY_RESPONSE 调用。 */
ucn_v6_result_t ucn_v6_runtime_time_complete(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_security_open_result_t *opened_delay_response,
    const ucn_v6_driver_rx_view_t *rx,
    uint64_t now_us);
#endif

/* EN: Copies bounded Runtime diagnostics without exposing private layout.
 * 中文：复制有界 Runtime 诊断，不暴露私有布局。 */
ucn_v6_result_t ucn_v6_runtime_copy_view(
    const ucn_v6_runtime_owner_t *runtime, ucn_v6_runtime_view_t *view);

#ifdef __cplusplus
}
#endif

#endif
