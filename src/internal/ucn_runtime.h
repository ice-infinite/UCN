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

/* TX produces a small action; retirement runs after the worker returns.
 * TX先产生小型事实，再在同一次Owner调度中退休，避免嵌套两套大型调用栈。 */
typedef struct ucn_i_tx_action {
    ucn_result_t result;
    uint16_t slot;
    uint8_t kind;
} ucn_i_tx_action_t;

/* Core-only phase: caller already owns the ucn_step API lease.
 * 仅Core调度阶段使用；调用方必须已持有ucn_step门禁，不是新公共入口。 */
bool ucn_i_node_tx_submit(ucn_node_t *node, uint16_t slot,
                          ucn_i_tx_action_t *action);

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
    /* EN: A composed Runtime owns Wire demultiplexing above Foundation.  In
     * that mode the C1 worker must never claim C0 or another Contract from
     * the shared Adapter queue.  Standalone ucn_node_t keeps the legacy
     * consume-and-drop behavior for unknown Contracts.
     * 中文：组合 Runtime 在 Foundation 之上统一拥有 Wire 分流。启用后，
     * C1 worker 绝不能从共享 Adapter 队列认领 C0 或其他 Contract；独立
     * ucn_node_t 仍保留对未知 Contract 的消费丢弃行为。 */
    uint8_t external_ingress_demux;
    uint8_t faulted;
    uint8_t time_initialized;
    uint8_t work_cursor;
    uint8_t scheduler_cursor;
    uint8_t queue_cursor[UCN_TRAFFIC_CLASS_COUNT];
    uint8_t queue_allocate_cursor[UCN_TRAFFIC_CLASS_COUNT];
    ucn_i_lock_ops_t lock;
    ucn_i_callback_gate_t callback_gate;
    ucn_i_callback_read_snapshot_t callback_read_snapshot;
    /* EN: Foundation links have no reopen operation in the simplified
     * Runtime.  Cache their immutable handles at init so an Endpoint callback
     * can build its read-only snapshot without nesting Adapter validation and
     * locking on the MCU callback stack.
     * 中文：简化 Runtime 的 Foundation Link 没有 reopen 操作，因此在 init
     * 时缓存其不可变 Handle，让 Endpoint callback 构建只读快照时无需在 MCU
     * 回调栈上再次嵌套 Adapter 校验与加锁。 */
    ucn_link_handle_t link_handles[UCN_LINK_COUNT];
    ucn_i_binding_t bindings[UCN_BINDING_COUNT];
    ucn_i_endpoint_t endpoints[UCN_ENDPOINT_COUNT];
    ucn_i_static_path_t paths[UCN_STATIC_PATH_COUNT];
    ucn_i_request_t requests[UCN_REQUEST_COUNT];
    ucn_i_receipt_t receipts[UCN_RECEIPT_COUNT];
    ucn_i_attempt_t attempts[UCN_ATTEMPT_COUNT];
    ucn_i_buffer_obligation_t buffers[UCN_BUFFER_OBLIGATION_COUNT];
    ucn_i_tx_slot_t tx_slots[UCN_TX_SLOT_COUNT];
    /* EN: One gated receive transaction, not another RX queue. The Adapter
     * retains the frame/token until retire; no concurrent Core API may reuse
     * this scratch while decoding or inside the Endpoint callback.
     * 中文：唯一受门控接收事务的工作区，不是新增队列；Adapter持有原帧/Token
     * 直到退休，解码/Endpoint回调期间禁止其他Core入口复用。 */
    struct {
        ucn_i_adapter_rx_view_t view;
        ucn_i_c1_frame_t frame;
    } rx_staging;
    ucn_i_adapter_t adapter;
    ucn_stats_t stats;
};

/* EN: Composition owns the Core owner identity.  The public initializer keeps
 * the standalone owner id, while the private initializer binds every handle
 * emitted by the Foundation to the exact Composition registry record.
 * 中文：Composition 拥有 Core Owner 身份。公共初始化仍使用独立运行时的固定
 * Owner ID；私有初始化则把 Foundation 产生的所有句柄绑定到 Registry 中的精确
 * Owner 记录。 */
ucn_result_t ucn_i_node_init_with_owner_instance(
    void *storage,
    size_t storage_bytes,
    const ucn_config_t *config,
    const ucn_ports_t *ports,
    uint16_t owner_instance,
    ucn_node_t **out_node);

/* EN: STOPPING has a narrow worker that cannot reach endpoint callbacks or
 * ordinary TX scheduling.  Composition uses it so shutdown has an honest
 * bounded MCU call chain instead of relying on a runtime branch in ucn_step.
 * 中文：STOPPING 使用一条不可进入 Endpoint 回调或普通 TX 调度的窄工作路径。
 * Composition 通过该入口获得真实有界的 MCU 关闭调用链，而不是依赖
 * ucn_step 中的运行时分支。 */
ucn_result_t ucn_i_node_stop_step(
    ucn_node_t *node,
    uint64_t now_us,
    const ucn_step_budget_t *budget,
    ucn_step_result_t *out_result);

#endif
