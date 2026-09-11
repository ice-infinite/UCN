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
#include "ucn/v6/ucn_v6_security.h"
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

typedef enum ucn_v6_runtime_bootstrap_ingress_kind {
    UCN_V6_RUNTIME_BOOTSTRAP_HELLO = 1,
    UCN_V6_RUNTIME_BOOTSTRAP_COOKIE_CHALLENGE = 2,
    UCN_V6_RUNTIME_BOOTSTRAP_HELLO_COOKIE = 3,
    UCN_V6_RUNTIME_BOOTSTRAP_EVENT = 4
} ucn_v6_runtime_bootstrap_ingress_kind_t;

typedef enum ucn_v6_runtime_bootstrap_response_kind {
    UCN_V6_RUNTIME_BOOTSTRAP_RESPONSE_NONE = 0,
    UCN_V6_RUNTIME_BOOTSTRAP_RESPONSE_COOKIE_CHALLENGE = 1,
    UCN_V6_RUNTIME_BOOTSTRAP_RESPONSE_HELLO_COOKIE = 2,
    UCN_V6_RUNTIME_BOOTSTRAP_RESPONSE_EVENT = 3
} ucn_v6_runtime_bootstrap_response_kind_t;

/* EN: Trusted, typed Bootstrap input produced only after Runtime has decoded
 * the exact pre-session Wire contract.  EVENT is emitted only after all
 * fragments match one already-open Bootstrap pending transaction.
 * 中文：仅在 Runtime 解码精确 Session 前 Wire 合同后生成的可信具名输入。
 * EVENT 只有在全部分片匹配已打开的 Bootstrap pending 后才会产生。 */
typedef struct ucn_v6_runtime_bootstrap_ingress {
    ucn_v6_runtime_bootstrap_ingress_kind_t kind;
    ucn_v6_address_class_t address_class;
    uint32_t realm_id;
    ucn_v6_binding_key_t source_binding;
    ucn_v6_bootstrap_key_t key;
    ucn_v6_bootstrap_pending_t pending;
    ucn_v6_bootstrap_event_t event;
    union {
        ucn_v6_bootstrap_hello_t hello;
        ucn_v6_bootstrap_cookie_challenge_t cookie_challenge;
        ucn_v6_bootstrap_hello_cookie_t hello_cookie;
        struct {
            ucn_v6_bootstrap_transcript_t transcript;
            ucn_v6_bootstrap_evidence_t evidence;
        } authenticated_event;
    } value;
} ucn_v6_runtime_bootstrap_ingress_t;

/* EN: Pure policy/proof result returned to Runtime.  Product code may select
 * credentials and address policy, but Runtime validates the response against
 * the unique Bootstrap FSM, performs durable Security commit, and owns TX.
 * `open_pending` is legal only for COOKIE_CHALLENGE/HELLO_COOKIE input.
 * 中文：产品返回给 Runtime 的纯策略/证明结果。产品可选择凭据和地址策略，
 * 但 Runtime 会用唯一 Bootstrap FSM 校验响应、执行持久 Security Commit
 * 并持有 TX。open_pending 仅允许用于 COOKIE_CHALLENGE/HELLO_COOKIE 输入。 */
typedef struct ucn_v6_runtime_bootstrap_action {
    bool open_pending;
    ucn_v6_bootstrap_transcript_t open_transcript;
    bool has_existing_binding;
    ucn_v6_binding_key_t existing_binding;
    ucn_v6_runtime_bootstrap_response_kind_t response_kind;
    ucn_v6_bootstrap_cookie_challenge_t cookie_challenge;
    ucn_v6_bootstrap_hello_cookie_t hello_cookie;
    ucn_v6_bootstrap_event_t response_event;
    ucn_v6_bootstrap_transcript_t response_transcript;
    ucn_v6_bootstrap_evidence_t response_evidence;
    bool commit_session;
} ucn_v6_runtime_bootstrap_action_t;

typedef struct ucn_v6_runtime_bootstrap_ops {
    void *context;
    /* The callback is a bounded proof/policy builder: it must not call any
     * Runtime/Owner API and must not perform externally visible side effects.
     * Callback rejection leaves protocol state unchanged.
     * 本回调只能有界生成证明/策略：不得重入 Runtime/Owner，也不得产生外部
     * 可见副作用；回调拒绝时协议状态保持不变。 */
    ucn_v6_result_t (*process_ingress)(
        void *context, uint64_t now_us,
        const ucn_v6_runtime_bootstrap_ingress_t *ingress,
        ucn_v6_runtime_bootstrap_action_t *action);
    /* Called by Runtime only when the exact incoming or outgoing event reaches
     * FINAL_DURABLE. The result is consumed synchronously by
     * ucn_v6_security_commit_join(); pointers inside it must remain valid for
     * the callback return's immediate caller only.
     * 仅在精确入站或出站事件到达 FINAL_DURABLE 时由 Runtime 调用；结果立即
     * 同步交给 ucn_v6_security_commit_join()，其中指针只需在该调用期间有效。 */
    ucn_v6_result_t (*build_join_commit)(
        void *context, uint64_t now_us,
        const ucn_v6_bootstrap_key_t *key,
        const ucn_v6_bootstrap_transcript_t *transcript,
        ucn_v6_join_commit_t *commit);
} ucn_v6_runtime_bootstrap_ops_t;

typedef struct ucn_v6_runtime_app_ops {
    void *context;
    /* Called only after Runtime has completed Wire decode, exact Link/Session
     * lookup, Hop authentication, replay admission, E2E open and exact ACL.
     * The callback never receives raw unauthenticated bytes. CONSUMED/DROP
     * retires the exact Adapter item; RETRY preserves this already-opened DTO
     * in Runtime and must represent bounded backpressure.
     * 仅在 Runtime 完成 Wire 解码、精确 Link/Session 定位、Hop 认证、
     * Replay 准入、E2E 打开与精确 ACL 后调用；回调永远不接收未认证
     * 原始字节。CONSUMED/DROP 退休精确 Adapter 项；RETRY 在 Runtime
     * 中保留已打开 DTO，且只能表达有界背压。 */
    ucn_v6_result_t (*handle_authenticated_ingress)(
        void *context, ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
        const ucn_v6_security_open_result_t *opened,
        const ucn_v6_driver_rx_view_t *rx,
        ucn_v6_runtime_ingress_disposition_t *disposition);
    /* Delivers one fully authenticated and reassembled Transfer payload. The
     * payload view is borrowed for this callback only. RETRY keeps the fixed
     * Transfer slot; CONSUMED/DROP retires it exactly once.
     * 投递一个已完成认证并重组的 Transfer Payload。视图仅在回调期间借用；
     * RETRY 保留固定 Transfer 槽，CONSUMED/DROP 精确退休一次。 */
    ucn_v6_result_t (*handle_reassembled_transfer)(
        void *context, ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
        const ucn_v6_transfer_completed_view_t *transfer,
        ucn_v6_runtime_ingress_disposition_t *disposition);
    /* Delivers an authenticated application RESULT control record. Transfer
     * transport validates and decodes it before this callback.
     * 投递已认证的应用 RESULT 控制记录；Transfer 在回调前完成结构校验与解码。 */
    ucn_v6_result_t (*handle_transfer_result)(
        void *context, ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
        const ucn_v6_transfer_result_t *transfer_result,
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
    /* Relay policy and the conservative per-Hop scheduling residence/TX
     * bounds. Both bounds are mandatory even when current traffic carries no
     * Hop Budget, so enabling budgeted traffic cannot silently use zero cost.
     * 中继选路策略，以及保守的逐跳排队驻留/TX 上界。即使当前流量没有携带
     * Hop Budget，两项上界也必须非零，避免后续启用预算流量时静默零扣减。 */
    ucn_v6_route_policy_t relay_route_policy;
    uint64_t relay_residence_bound_us;
    uint64_t relay_transmit_bound_us;
    /* Maximum authenticated credit a peer may advertise to the local
     * Transfer Owner. Zero is invalid; the peer cannot enlarge this product
     * policy through Wire input.
     * 对端可向本机 Transfer Owner 通告的最大认证 Credit。0 非法；对端
     * 不能通过 Wire 输入放大本产品策略。 */
    uint16_t transfer_maximum_credit;
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
    ucn_v6_runtime_bootstrap_ops_t bootstrap_ops;
    ucn_v6_runtime_app_ops_t app;
} ucn_v6_runtime_config_t;

typedef union ucn_v6_runtime_owner_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_RUNTIME_OWNER_STORAGE_BYTES];
} ucn_v6_runtime_owner_storage_t;

typedef struct ucn_v6_runtime_view {
    uint32_t rx_consumed;
    uint32_t rx_relayed;
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
    uint32_t capability_frames_consumed;
    uint32_t capability_queries_sent;
    uint32_t capability_advertisements_sent;
    uint32_t transfer_frames_consumed;
    uint32_t transfer_messages_started;
    uint32_t transfer_messages_delivered;
    uint32_t transfer_messages_retired;
    uint32_t transfer_control_frames_sent;
    uint32_t protocol_frames_rejected;
    uint32_t bootstrap_frames_consumed;
    uint32_t bootstrap_frames_sent;
    uint32_t bootstrap_sessions_committed;
    uint32_t bootstrap_objects_expired;
    ucn_v6_result_t last_protocol_error;
    bool faulted;
} ucn_v6_runtime_view_t;

typedef struct ucn_v6_runtime_send_request {
    /* Semantic frame before Route/Path and security mutable fields are
     * installed. Payload is borrowed only for the duration of this call.
     * Route Domain is the authoritative endpoint identity.
     * 安装 Route/Path 与安全可变字段前的语义 Frame。Payload 仅在本次调用
     * 期间借用；Route Domain 是端到端身份的权威来源。 */
    ucn_v6_frame_t frame;
    ucn_v6_route_select_request_t route;
    uint64_t buffer_token;
    uint8_t local_priority;
    bool request_timestamp;
} ucn_v6_runtime_send_request_t;

typedef struct ucn_v6_runtime_send_result {
    /* Read-only admission snapshot. QOS_TX resolves the Route again before
     * security sequence reservation and may use a newer valid generation.
     * 只读准入快照；QOS_TX 在预留安全序号前重新解析 Route，可能使用更新且
     * 合法的代际。 */
    ucn_v6_route_path_ref_t admission_route_ref;
    uint16_t estimated_encoded_length;
} ucn_v6_runtime_send_result_t;

/* Canonical large-message TX request. Route identity and application payload
 * remain caller-owned until the final buffer release. Runtime derives the
 * smallest valid Address Class and exact Hop limit from the frozen Route.
 * 大消息唯一 TX 请求。Route 身份与业务 Payload 在最终返还前由调用方持有；
 * Runtime 从冻结 Route 推导最小合法 Address Class 与精确 Hop limit。 */
typedef struct ucn_v6_runtime_transfer_send_request {
    ucn_v6_transfer_send_request_t transfer;
    bool has_hop_budget;
    uint64_t initial_hop_budget_us;
    uint64_t remaining_hop_budget_us;
    uint8_t local_priority;
} ucn_v6_runtime_transfer_send_request_t;

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

/* Starts the device side of exactly one link-local JOIN or REAUTH exchange.
 * Runtime freezes the physical Peer discriminator and exact Link generation,
 * emits the canonical HELLO itself, and accepts a Cookie Challenge only when
 * it matches this live initiation record.  No application buffer is borrowed.
 * 启动一个精确的链路本地 JOIN 或 REAUTH 设备侧事务。Runtime 冻结物理 Peer
 * 区分值和精确 Link 代际，自行发送规范 HELLO；仅匹配该活跃发起记录的
 * Cookie Challenge 才会被接受，且不会借用应用 Buffer。 */
ucn_v6_result_t ucn_v6_runtime_bootstrap_start(
    ucn_v6_runtime_owner_t *runtime,
    uint64_t now_us,
    uint16_t link_id,
    uint32_t link_generation,
    uint32_t local_peer_discriminator,
    ucn_v6_address_class_t address_class,
    uint32_t realm_id,
    const ucn_v6_bootstrap_hello_t *hello);

/* EN: Canonical ordinary-frame TX entry. Runtime selects the live Route,
 * freezes Route/Path identity, applies E2E and next-Hop security, copies the
 * immutable encoded frame into fixed storage, then admits it to QoS. Physical
 * Adapter submission occurs only in the QOS_TX owner phase.
 * 中文：普通帧唯一规范 TX 入口。Runtime 选择活跃 Route、冻结 Route/Path
 * 身份、执行 E2E 与下一跳安全、把不可变编码帧复制进固定存储，再进入 QoS；
 * 物理 Adapter 提交只能发生在 QOS_TX Owner 阶段。 */
ucn_v6_result_t ucn_v6_runtime_send_frame(
    ucn_v6_runtime_owner_t *runtime,
    uint64_t now_us,
    const ucn_v6_runtime_send_request_t *request,
    ucn_v6_runtime_send_result_t *result);

/* Starts one Runtime-owned Selective-Repeat transfer. Fragment frames are
 * generated, secured, scheduled and submitted only by owner phases; the
 * caller receives the original buffer token after remote reassembly or a
 * terminal failure.
 * 启动由 Runtime 持有的选择重传事务。分片只能由 Owner 阶段生成、保护、调度
 * 和提交；远端完成重组或发生终态失败后返还原始 Buffer token。 */
ucn_v6_result_t ucn_v6_runtime_transfer_send(
    ucn_v6_runtime_owner_t *runtime,
    uint64_t now_us,
    const ucn_v6_runtime_transfer_send_request_t *request);

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

/* Must be called from handle_authenticated_ingress for the exact active
 * TIME_SYNC Adapter RX
 * item.  Runtime verifies and freezes the reverse Path, captures T2, and
 * returns the only handle that may enqueue its DELAY_REQUEST.
 * 必须在 handle_authenticated_ingress 内针对精确 TIME_SYNC
 * Adapter RX 项调用。Runtime
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

/* Must be called from handle_authenticated_ingress for an authenticated
 * DELAY_REQUEST.
 * Runtime matches the Master transaction and actual T4 RX event, then builds,
 * protects and enqueues the exact T1/T4 DELAY_RESPONSE on the frozen forward
 * Path.  Duplicate requests replay the same semantic T1/T4 response.
 * 必须在 handle_authenticated_ingress 内处理认证
 * DELAY_REQUEST。Runtime 匹配 Master 事务
 * 与真实 T4 RX 事件，再在冻结正向 Path 上构造、保护并排队精确 T1/T4 响应；
 * 重复请求只重发同一语义响应。 */
ucn_v6_result_t ucn_v6_runtime_time_respond_delay_request(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_security_open_result_t *opened_delay_request,
    const ucn_v6_driver_rx_view_t *rx,
    uint64_t buffer_token,
    uint64_t now_us);

/* Must be called from handle_authenticated_ingress for authenticated
 * DELAY_RESPONSE.
 * 必须在 handle_authenticated_ingress 中针对认证
 * DELAY_RESPONSE 调用。 */
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
