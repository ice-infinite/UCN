#ifndef UCN_V6_MESSAGE_H
#define UCN_V6_MESSAGE_H

#include "ucn/v6/ucn_v6_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_OPERATION_DIGEST_BYTES ((size_t)32U)
#define UCN_V6_OPERATION_RESULT_MAX_BYTES ((size_t)64U)
#define UCN_V6_OPERATION_JOURNAL_SLOTS \
    ((size_t)UCN_V6_CONFIG_OPERATION_SLOTS)
#define UCN_V6_OPERATION_HIGH_WATER_SLOTS \
    ((size_t)UCN_V6_CONFIG_OPERATION_HIGH_WATERS)
#define UCN_V6_OPERATION_JOURNAL_MAGIC UINT32_C(0x554F5036)
#define UCN_V6_OPERATION_JOURNAL_SCHEMA UINT16_C(1)
#define UCN_V6_MESSAGE_WITNESS_MAGIC UINT32_C(0x554D5736)
#define UCN_V6_MESSAGE_WITNESS_SCHEMA UINT16_C(1)
#define UCN_V6_MESSAGE_DIGEST_PROVIDER_API_VERSION UINT16_C(1)
#define UCN_V6_MESSAGE_DIGEST_SHA256 UINT16_C(1)
#define UCN_V6_MESSAGE_LIFECYCLE_PROVIDER_API_VERSION UINT16_C(1)

enum {
    UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED = 1U << 0,
    UCN_V6_MESSAGE_WITNESS_ALLOCATOR_COMMISSIONED = 1U << 1
};

typedef enum ucn_v6_endpoint_execution_contract {
    UCN_V6_ENDPOINT_NON_RETRYABLE = 1,
    UCN_V6_ENDPOINT_IDEMPOTENT_REPLAYABLE = 2,
    UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE = 3
} ucn_v6_endpoint_execution_contract_t;

typedef enum ucn_v6_completion_level {
    UCN_V6_COMPLETION_LOCAL_ACCEPTED = 1,
    UCN_V6_COMPLETION_LINK_SUBMITTED = 2,
    UCN_V6_COMPLETION_REMOTE_REASSEMBLED = 3,
    UCN_V6_COMPLETION_REMOTE_INBOX_ACCEPTED = 4,
    UCN_V6_COMPLETION_APPLICATION_RESULT = 5
} ucn_v6_completion_level_t;

typedef struct ucn_v6_message_descriptor {
    ucn_v6_traffic_class_t traffic_class;
    ucn_v6_delivery_guarantee_t delivery_guarantee;
    ucn_v6_interaction_role_t interaction_role;
    uint16_t source_endpoint;
    uint16_t destination_endpoint;
    uint64_t operation_id;
    uint16_t payload_length;
} ucn_v6_message_descriptor_t;

typedef struct ucn_v6_endpoint_contract {
    uint16_t endpoint_id;
    uint8_t traffic_class_mask;
    uint8_t delivery_guarantee_mask;
    uint8_t interaction_role_mask;
    uint16_t max_payload_bytes;
    uint16_t max_result_bytes;
    ucn_v6_endpoint_execution_contract_t execution_contract;
} ucn_v6_endpoint_contract_t;

typedef struct ucn_v6_operation_key {
    ucn_v6_principal_t initiator_principal;
    uint64_t operation_id;
} ucn_v6_operation_key_t;

typedef enum ucn_v6_operation_phase {
    UCN_V6_OPERATION_PHASE_INVALID = 0,
    UCN_V6_OPERATION_PHASE_PREPARED = 1,
    UCN_V6_OPERATION_PHASE_EXECUTING = 2,
    UCN_V6_OPERATION_PHASE_COMMITTED_RESULT = 3,
    UCN_V6_OPERATION_PHASE_IN_DOUBT = 4,
    UCN_V6_OPERATION_PHASE_TOMBSTONED = 5
} ucn_v6_operation_phase_t;

typedef enum ucn_v6_operation_admission {
    UCN_V6_OPERATION_ADMISSION_NEW = 1,
    UCN_V6_OPERATION_ADMISSION_PREPARED = 2,
    UCN_V6_OPERATION_ADMISSION_EXECUTING = 3,
    UCN_V6_OPERATION_ADMISSION_RESULT_REPLAY = 4,
    UCN_V6_OPERATION_ADMISSION_IN_DOUBT = 5,
    UCN_V6_OPERATION_ADMISSION_TOMBSTONED = 6
} ucn_v6_operation_admission_t;

typedef struct ucn_v6_operation_slot {
    bool occupied;
    ucn_v6_operation_key_t key;
    uint16_t endpoint_id;
    ucn_v6_endpoint_execution_contract_t execution_contract;
    uint8_t request_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    ucn_v6_operation_phase_t phase;
    int32_t result_code;
    uint16_t result_length;
    uint8_t result[UCN_V6_OPERATION_RESULT_MAX_BYTES];
    uint8_t result_digest[UCN_V6_OPERATION_DIGEST_BYTES];
} ucn_v6_operation_slot_t;

/* EN: Fixed per-Principal replay floor. Slots are never evicted or reused
 * implicitly: a full table fails with UCN_V6_ERR_NO_SPACE. Reuse is possible
 * only through ucn_v6_operation_release_principal_history() after the trusted
 * lifecycle Provider proves permanent Principal retirement.
 * 中文：固定容量的逐 Principal 重放高水位。表项绝不隐式驱逐或复用；表满
 * 返回 UCN_V6_ERR_NO_SPACE。只有可信生命周期 Provider 证明 Principal 永久
 * 退休后，才可通过 ucn_v6_operation_release_principal_history() 复用。 */
typedef struct ucn_v6_operation_high_water {
    bool occupied;
    ucn_v6_principal_t initiator_principal;
    uint64_t retired_through_operation_id;
} ucn_v6_operation_high_water_t;

typedef struct ucn_v6_operation_journal_snapshot {
    uint32_t magic;
    uint16_t schema;
    uint16_t slot_count;
    uint64_t snapshot_generation;
    ucn_v6_operation_slot_t slots[UCN_V6_OPERATION_JOURNAL_SLOTS];
    ucn_v6_operation_high_water_t
        high_waters[UCN_V6_OPERATION_HIGH_WATER_SLOTS];
} ucn_v6_operation_journal_snapshot_t;

typedef enum ucn_v6_operation_reconciliation_outcome {
    UCN_V6_OPERATION_RECONCILIATION_RESULT = 1,
    UCN_V6_OPERATION_RECONCILIATION_TOMBSTONE = 2
} ucn_v6_operation_reconciliation_outcome_t;

/* EN: Canonical output of an authenticated external reconciliation.  A
 * RESULT is re-digested by Message before it is committed.  A TOMBSTONE has
 * no result bytes and carries the authenticated terminal digest supplied by
 * the lifecycle Provider.
 * 中文：经认证外部对账的规范输出。RESULT 在提交前由 Message 重新计算摘要；
 * TOMBSTONE 不携带结果字节，其 terminal_digest 必须来自生命周期 Provider
 * 已验证的终态证明。 */
typedef struct ucn_v6_operation_reconciliation {
    ucn_v6_operation_reconciliation_outcome_t outcome;
    int32_t result_code;
    uint16_t result_length;
    uint8_t result[UCN_V6_OPERATION_RESULT_MAX_BYTES];
    uint8_t terminal_digest[UCN_V6_OPERATION_DIGEST_BYTES];
} ucn_v6_operation_reconciliation_t;

/* EN: Trusted, caller-owned lifecycle Provider.  The Provider object and its
 * context must outlive the Journal. Callbacks execute under the Journal
 * callback gate and must not re-enter protocol APIs sharing that gate.
 * Returning UCN_V6_OK is an assertion that the Provider has verified
 * the named durable/authenticated condition; ordinary application booleans
 * and timestamps are deliberately not accepted by the Journal API. These
 * callbacks are repeatable proof queries: they must not consume the proof or
 * perform a new irreversible side effect, because Journal persistence can
 * fail after a successful query and retry the same query later.
 *
 * authorize_prepared_abort verifies the product cancellation/timeout policy
 * and supplies its terminal code and digest. authorize_result_retirement
 * verifies both an authenticated result ACK and the minimum retention window.
 * authorize_tombstone_reclaim verifies the maximum replay lifetime and
 * returns an independently durable initiator high-water strictly beyond the
 * tombstoned operation. authorize_history_release is stronger: it proves
 * that the Principal is permanently unable to authenticate again and that
 * every replay window has elapsed, permitting reuse of its fixed high-water
 * slot.
 *
 * 中文：可信、由调用方持有的生命周期 Provider。所有回调都在 Journal 共享
 * 回调门内执行，不得重入使用同一门的协议 API。回调返回 UCN_V6_OK 表示它
 * 已验证对应的持久化/认证条件；Journal 不再接受普通业务层 bool 或时间戳冒充
 * 安全证明。回调必须是可重复的证明查询，不得消费证明或新触发不可逆副作用，
 * 因为查询成功后 Journal 仍可能持久化失败并在稍后重试。
 * authorize_history_release 仅能在 Principal 永久失去认证资格且全部重放窗口
 * 结束后成功，从而安全复用固定容量的高水位槽。 */
typedef struct ucn_v6_message_lifecycle_ops {
    void *context;
    uint16_t api_version;
    ucn_v6_result_t (*authorize_prepared_abort)(
        void *context,
        const ucn_v6_operation_slot_t *durable_slot,
        int32_t *terminal_result_code,
        uint8_t terminal_digest[UCN_V6_OPERATION_DIGEST_BYTES]);
    ucn_v6_result_t (*reconcile_in_doubt)(
        void *context,
        const ucn_v6_operation_slot_t *durable_slot,
        ucn_v6_operation_reconciliation_t *reconciliation);
    ucn_v6_result_t (*authorize_result_retirement)(
        void *context,
        const ucn_v6_operation_slot_t *durable_slot);
    ucn_v6_result_t (*authorize_tombstone_reclaim)(
        void *context,
        const ucn_v6_operation_slot_t *durable_slot,
        uint64_t *durable_initiator_high_water);
    ucn_v6_result_t (*authorize_history_release)(
        void *context,
        const ucn_v6_operation_high_water_t *durable_high_water);
} ucn_v6_message_lifecycle_ops_t;

/* Independent anti-rollback witness.  A conforming Provider stores this in
 * a monotonic/fenced domain separate from the dual-slot journal.  Once either
 * commissioned bit is set, load_witness must never report NOT_FOUND. */
typedef struct ucn_v6_message_witness {
    uint32_t magic;
    uint16_t schema;
    uint16_t flags;
    uint64_t witness_generation;
    uint64_t journal_committed_generation;
    uint64_t journal_pending_generation;
    uint64_t operation_id_high_water;
} ucn_v6_message_witness_t;

typedef struct ucn_v6_message_store_ops {
    void *context;
    ucn_v6_result_t (*load_witness)(
        void *context,
        ucn_v6_message_witness_t *witness);
    ucn_v6_result_t (*reserve_witness)(
        void *context,
        const ucn_v6_message_witness_t *witness);
    ucn_v6_result_t (*load_journal)(
        void *context,
        ucn_v6_operation_journal_snapshot_t *snapshot);
    ucn_v6_result_t (*submit_journal)(
        void *context,
        const ucn_v6_operation_journal_snapshot_t *snapshot);
    ucn_v6_message_lifecycle_ops_t lifecycle;
} ucn_v6_message_store_ops_t;

typedef struct ucn_v6_operation_id_allocator ucn_v6_operation_id_allocator_t;
typedef struct ucn_v6_operation_journal ucn_v6_operation_journal_t;
#ifndef UCN_V6_OPERATION_ID_ALLOCATOR_STORAGE_BYTES
#define UCN_V6_OPERATION_ID_ALLOCATOR_STORAGE_BYTES ((size_t)256U)
#endif
#ifndef UCN_V6_OPERATION_JOURNAL_STORAGE_BYTES
#define UCN_V6_OPERATION_JOURNAL_STORAGE_BYTES                         \
    ((size_t)(1024U + 3U * (UCN_V6_CONFIG_OPERATION_SLOTS * 256U +   \
                            UCN_V6_CONFIG_OPERATION_HIGH_WATERS * 64U)))
#endif
typedef union ucn_v6_operation_id_allocator_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_OPERATION_ID_ALLOCATOR_STORAGE_BYTES];
} ucn_v6_operation_id_allocator_storage_t;
typedef union ucn_v6_operation_journal_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_OPERATION_JOURNAL_STORAGE_BYTES];
} ucn_v6_operation_journal_storage_t;

typedef struct ucn_v6_operation_id_allocator_view {
    uint64_t next_id;
    uint64_t reserved_through;
    uint64_t witness_generation;
    uint32_t reservation_block_size;
    bool faulted;
} ucn_v6_operation_id_allocator_view_t;

typedef struct ucn_v6_operation_journal_view {
    uint64_t committed_generation;
    uint64_t witness_generation;
    uint64_t pending_generation;
    uint16_t occupied_slots;
    uint16_t occupied_high_waters;
    bool faulted;
} ucn_v6_operation_journal_view_t;

typedef enum ucn_v6_endpoint_dispatch_action {
    UCN_V6_ENDPOINT_DISPATCH_EXECUTED = 1,
    UCN_V6_ENDPOINT_DISPATCH_RESULT_REPLAY = 2,
    UCN_V6_ENDPOINT_DISPATCH_IN_DOUBT = 3,
    UCN_V6_ENDPOINT_DISPATCH_TOMBSTONED = 4
} ucn_v6_endpoint_dispatch_action_t;

typedef struct ucn_v6_endpoint_execution_result {
    int32_t result_code;
    uint16_t result_length;
    uint8_t result[UCN_V6_OPERATION_RESULT_MAX_BYTES];
} ucn_v6_endpoint_execution_result_t;

typedef struct ucn_v6_endpoint_dispatch_result {
    ucn_v6_endpoint_dispatch_action_t action;
    int32_t result_code;
    uint16_t result_length;
    uint8_t result[UCN_V6_OPERATION_RESULT_MAX_BYTES];
    uint8_t result_digest[UCN_V6_OPERATION_DIGEST_BYTES];
} ucn_v6_endpoint_dispatch_result_t;

typedef ucn_v6_result_t (*ucn_v6_endpoint_execute_fn)(
    void *context,
    const ucn_v6_message_descriptor_t *message,
    const uint8_t *payload,
    uint16_t payload_length,
    ucn_v6_endpoint_execution_result_t *result);

/* EN: For DURABLE_AT_MOST_ONCE dispatch, the execution callback runs under
 * the Journal callback gate after EXECUTING is durable. It must not re-enter
 * any protocol API sharing that gate; such attempts fail closed.
 * 中文：DURABLE_AT_MOST_ONCE 分派会在 EXECUTING 落盘后，于 Journal 回调门内
 * 执行业务回调。回调不得重入任何共享该门的协议 API；重入尝试将失败关闭。 */

/* EN: Trusted product digest Provider. The dispatcher, not the application,
 * constructs the canonical fixed prefix and supplies the variable payload as
 * a second segment. Version 1 requires SHA-256(prefix || variable), exactly
 * UCN_V6_OPERATION_DIGEST_BYTES bytes, and deterministic output.  The
 * callback executes under the Journal callback gate; recursive protocol or
 * persistence entry must fail closed.
 * 中文：可信的产品摘要 Provider。规范固定前缀由 Dispatcher 内部构造，变长
 * Payload 作为第二段传入；版本 1 要求 compute 计算确定性的
 * SHA-256(prefix || variable)，并精确输出 UCN_V6_OPERATION_DIGEST_BYTES
 * 字节。回调在 Journal 回调门内执行，递归进入协议或持久化接口必须失败关闭。 */
typedef ucn_v6_result_t (*ucn_v6_message_digest_compute_fn)(
    void *context,
    const uint8_t *canonical_prefix,
    size_t canonical_prefix_length,
    const uint8_t *variable_bytes,
    size_t variable_length,
    uint8_t digest[UCN_V6_OPERATION_DIGEST_BYTES]);

typedef struct ucn_v6_message_digest_ops {
    void *context;
    uint16_t api_version;
    uint16_t algorithm_id;
    uint16_t digest_bytes;
    ucn_v6_message_digest_compute_fn compute;
} ucn_v6_message_digest_ops_t;

/* Canonical digest inputs (no padding, all integers big-endian):
 * REQUEST = "UCN6REQ\x01" || authenticated principal[16] || endpoint
 * {id,u8 masks,max payload,max result,u8 execution contract} || descriptor
 * {u8 traffic,u8 delivery,u8 interaction,source endpoint,destination
 * endpoint,operation id,payload length} || payload bytes.
 * RESULT = "UCN6RES\x01" || request digest[32] || result code as uint32 ||
 * result length || result bytes.
 * 规范摘要输入（无填充，全部整数为大端）：REQUEST 绑定认证 Principal、完整
 * Endpoint 合同、完整 Message Descriptor、Payload 长度与字节；RESULT 绑定
 * Request Digest、结果码、结果长度与结果字节。 */

struct ucn_v6_feature_manifest;

/* EN: Validates orthogonal Traffic, delivery and interaction fields against
 * one immutable Endpoint contract.
 * 中文：按不可变 Endpoint 合同校验彼此正交的流量、交付和交互字段。 */
ucn_v6_result_t ucn_v6_message_validate(
    const ucn_v6_message_descriptor_t *message,
    const ucn_v6_endpoint_contract_t *endpoint);

/* EN: Validates the exact one-step monotonic witness transition that a
 * persistence Provider must enforce atomically.
 * 中文：校验持久化 Provider 必须原子执行的单步 Witness 单调转换。 */
ucn_v6_result_t ucn_v6_message_witness_transition_validate(
    const ucn_v6_message_witness_t *previous,
    const ucn_v6_message_witness_t *next);

/* EN: Initializes a persistently reserved, non-wrapping Operation-ID source.
 * 中文：初始化持久区间预留且不可回绕的 Operation ID 分配器。 */
ucn_v6_result_t ucn_v6_operation_id_allocator_init_in_place(
    void *storage,
    size_t storage_bytes,
    const struct ucn_v6_feature_manifest *manifest,
    const ucn_v6_message_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate,
    uint32_t reservation_block_size,
    ucn_v6_operation_id_allocator_t **allocator);
ucn_v6_result_t ucn_v6_operation_id_take(
    ucn_v6_operation_id_allocator_t *allocator,
    uint64_t *operation_id);
ucn_v6_result_t ucn_v6_operation_id_allocator_copy_view(
    const ucn_v6_operation_id_allocator_t *allocator,
    ucn_v6_operation_id_allocator_view_t *view);

/* EN: Loads the fixed journal and atomically migrates crash-time EXECUTING
 * entries to IN_DOUBT before accepting work.
 * 中文：加载固定 Journal，并在接收工作前原子地把掉电时 EXECUTING 项迁移为
 * IN_DOUBT。 */
ucn_v6_result_t ucn_v6_operation_journal_init_in_place(
    void *storage,
    size_t storage_bytes,
    const struct ucn_v6_feature_manifest *manifest,
    const ucn_v6_message_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_operation_journal_t **journal);
ucn_v6_result_t ucn_v6_operation_prepare(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    uint16_t endpoint_id,
    ucn_v6_endpoint_execution_contract_t execution_contract,
    const uint8_t request_digest[UCN_V6_OPERATION_DIGEST_BYTES],
    ucn_v6_operation_admission_t *admission);
ucn_v6_result_t ucn_v6_operation_mark_executing(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key);
ucn_v6_result_t ucn_v6_operation_mark_in_doubt(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key);
ucn_v6_result_t ucn_v6_operation_commit_result(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    int32_t result_code,
    const uint8_t *result,
    uint16_t result_length,
    const uint8_t result_digest[UCN_V6_OPERATION_DIGEST_BYTES]);
ucn_v6_result_t ucn_v6_operation_resolve_in_doubt(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    const ucn_v6_message_digest_ops_t *digest_ops);
ucn_v6_result_t ucn_v6_operation_abort_prepared(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key);
ucn_v6_result_t ucn_v6_operation_tombstone_result(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key);
ucn_v6_result_t ucn_v6_operation_reclaim_tombstone(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key);
/* EN: Reuses one fixed high-water slot only after the lifecycle Provider has
 * proved permanent Principal retirement and the absence of live slots.
 * 中文：只有生命周期 Provider 证明 Principal 永久退休且已无在用操作槽后，
 * 才允许复用一个固定容量的高水位槽。 */
ucn_v6_result_t ucn_v6_operation_release_principal_history(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_principal_t *initiator_principal);
ucn_v6_result_t ucn_v6_operation_copy_slot(
    const ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    ucn_v6_operation_slot_t *slot);
ucn_v6_result_t ucn_v6_operation_journal_copy_view(
    const ucn_v6_operation_journal_t *journal,
    ucn_v6_operation_journal_view_t *view);

/* EN: Executes one authenticated endpoint request through the declared
 * contract. DURABLE_AT_MOST_ONCE computes request/result digests internally
 * from canonical fields through a trusted digest Provider, persists PREPARED
 * and EXECUTING before calling application code, then persists the result
 * before exposing it; uncertain execution is never repeated automatically.
 * 中文：按声明的端点合同执行一条已认证请求。DURABLE_AT_MOST_ONCE 在调用
 * 可信摘要 Provider 上对规范字段内部计算请求/结果摘要，在调用业务代码前
 * 必先持久化 PREPARED 与 EXECUTING，并在对外返回前持久化结果；执行结果
 * 不确定时绝不自动重做。 */
ucn_v6_result_t ucn_v6_endpoint_dispatch_request(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_endpoint_contract_t *endpoint,
    const ucn_v6_message_descriptor_t *message,
    const ucn_v6_principal_t *authenticated_initiator,
    const uint8_t *payload,
    uint16_t payload_length,
    const ucn_v6_message_digest_ops_t *digest_ops,
    ucn_v6_endpoint_execute_fn execute,
    void *execute_context,
    ucn_v6_endpoint_dispatch_result_t *dispatch_result);

#ifdef __cplusplus
}
#endif

#endif
