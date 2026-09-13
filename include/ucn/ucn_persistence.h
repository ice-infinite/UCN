#ifndef UCN_PERSISTENCE_H
#define UCN_PERSISTENCE_H

#include "ucn/ucn_product.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_PERSIST_API_VERSION UINT16_C(1)
#define UCN_PERSIST_STORAGE_LAYOUT UINT16_C(1)
#define UCN_PERSIST_PROTOCOL_MANIFEST_VERSION UINT32_C(1)
#define UCN_PERSIST_RECORD_ENVELOPE_VERSION UINT16_C(1)
#define UCN_PERSIST_SLOT_COUNT 2U
#define UCN_PERSIST_ENVELOPE_BYTES 96U
#define UCN_PERSIST_COMMIT_MARKER_BYTES 16U
#define UCN_PERSIST_DIGEST_BYTES 16U
#define UCN_PERSIST_DIGEST_WORKSPACE_BYTES 256U
#define UCN_PERSIST_MAX_STEP_OPERATIONS 32U

#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_PERSIST_DOMAIN_COUNT 2U
#define UCN_PERSIST_BODY_BYTES 256U
#define UCN_PERSIST_STORAGE_BYTES 6144U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_PERSIST_DOMAIN_COUNT 4U
#define UCN_PERSIST_BODY_BYTES 512U
#define UCN_PERSIST_STORAGE_BYTES 12288U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define UCN_PERSIST_DOMAIN_COUNT 8U
#define UCN_PERSIST_BODY_BYTES 1024U
#define UCN_PERSIST_STORAGE_BYTES 32768U
#else
#error "UCN_PROFILE must be UCN_PROFILE_NANO, UCN_PROFILE_LITE, or UCN_PROFILE_FULL"
#endif

#define UCN_PERSIST_SLOT_BYTES                                             \
    (UCN_PERSIST_ENVELOPE_BYTES + UCN_PERSIST_BODY_BYTES +                \
     UCN_PERSIST_COMMIT_MARKER_BYTES)
#define UCN_PERSIST_STORAGE_ALIGNMENT 8U
#define UCN_PERSIST_GATE_STORAGE_BYTES 64U
#define UCN_PERSIST_GATE_STORAGE_ALIGNMENT 8U

typedef uint16_t ucn_persist_digest_suite_t;
enum {
    UCN_PERSIST_DIGEST_BLAKE2S_128 = 1
};

typedef uint8_t ucn_persist_witness_policy_t;
enum {
    UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC = 1
};

typedef uint8_t ucn_persist_provider_atomicity_t;
enum {
    UCN_PERSIST_ATOMIC_COMMIT_MARKER_16 = 1
};

UCN_STATIC_ASSERT(UCN_PERSIST_DOMAIN_COUNT > 0U &&
                      UCN_PERSIST_DOMAIN_COUNT <= 32U,
                  persist_domain_capacity_must_fit_required_mask);
UCN_STATIC_ASSERT(UCN_PERSIST_BODY_BYTES <= UINT32_MAX,
                  persist_body_capacity_must_fit_u32);

typedef union ucn_persistence_storage {
    uint64_t force_alignment;
    unsigned char bytes[UCN_PERSIST_STORAGE_BYTES];
} ucn_persistence_storage_t;

typedef union ucn_persist_gate_storage {
    uint64_t force_alignment;
    unsigned char bytes[UCN_PERSIST_GATE_STORAGE_BYTES];
} ucn_persist_gate_storage_t;

/* EN: Caller-owned scratch for canonical Manifest hashing. Keeping this
 * storage outside the call stack makes initialization safe on small MCU task
 * stacks while preserving reentrant, allocation-free hashing.
 * 中文：调用方持有的 Manifest 规范摘要临时区。它不占用函数调用栈，因此适合
 * 小栈 MCU，同时仍保持可重入、零动态内存。 */
typedef union ucn_persistence_digest_workspace {
    uint64_t force_alignment;
    unsigned char bytes[UCN_PERSIST_DIGEST_WORKSPACE_BYTES];
} ucn_persistence_digest_workspace_t;

#define UCN_DECLARE_PERSISTENCE_STORAGE(name_) ucn_persistence_storage_t name_
#define UCN_DECLARE_PERSIST_GATE_STORAGE(name_) ucn_persist_gate_storage_t name_
#define UCN_DECLARE_PERSIST_DIGEST_WORKSPACE(name_)                       \
    ucn_persistence_digest_workspace_t name_

struct ucn_persistence_owner;
struct ucn_persist_callback_gate;
typedef struct ucn_persistence_owner ucn_persistence_owner_t;
typedef struct ucn_persist_callback_gate ucn_persist_callback_gate_t;

typedef uint16_t ucn_persist_domain_kind_t;
enum {
    UCN_PERSIST_DOMAIN_IDENTITY_BINDING = 1,
    UCN_PERSIST_DOMAIN_SECURITY_HIGH_WATER = 2,
    UCN_PERSIST_DOMAIN_TRANSPORT_HIGH_WATER = 3,
    UCN_PERSIST_DOMAIN_OPERATION_JOURNAL = 4,
    UCN_PERSIST_DOMAIN_GROUP_POLICY_KEY = 5,
    UCN_PERSIST_DOMAIN_TIME_AUTHORITY = 6,
    UCN_PERSIST_DOMAIN_CLUSTER = 7,
    UCN_PERSIST_DOMAIN_PRODUCT_CONFIG = 8
};

typedef struct ucn_persist_domain_key {
    uint64_t domain_id;
    uint16_t domain_kind;
    uint16_t reserved_zero;
    uint32_t reserved_zero2;
} ucn_persist_domain_key_t;

typedef struct ucn_persist_manifest_entry {
    uint16_t struct_size;
    uint16_t api_version;
    ucn_persist_domain_key_t domain;
    uint32_t body_capacity_bytes;
    uint32_t slot_capacity_bytes;
    uint16_t schema_id;
    uint16_t schema_version;
    uint16_t digest_suite;
    uint8_t witness_policy;
    uint8_t provider_atomicity_class;
} ucn_persist_manifest_entry_t;

typedef struct ucn_persist_manifest {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t protocol_manifest_version;
    uint32_t storage_layout_version;
    uint64_t composition_feature_bits;
    const ucn_persist_manifest_entry_t *entries;
    uint16_t entry_count;
    uint8_t profile_id;
    uint8_t reserved_zero;
    uint8_t expected_digest[UCN_PERSIST_DIGEST_BYTES];
} ucn_persist_manifest_t;

typedef uint8_t ucn_persist_io_phase_t;
enum {
    UCN_PERSIST_IO_LOAD_SLOT = 1,
    UCN_PERSIST_IO_WRITE_INACTIVE = 2,
    UCN_PERSIST_IO_READBACK = 3,
    UCN_PERSIST_IO_PUBLISH_MARKER = 4,
    UCN_PERSIST_IO_LOAD_WITNESS = 5,
    UCN_PERSIST_IO_ADVANCE_WITNESS = 6
};

typedef uint8_t ucn_persist_io_start_t;
enum {
    UCN_PERSIST_IO_COMPLETED = 1,
    UCN_PERSIST_IO_PENDING = 2,
    UCN_PERSIST_IO_FAILED = 3
};

typedef uint8_t ucn_persist_blob_state_t;
enum {
    UCN_PERSIST_BLOB_PRESENT = 1,
    UCN_PERSIST_BLOB_EMPTY = 2,
    UCN_PERSIST_BLOB_FAULT = 3
};

/* EN: LOAD_SLOT always returns the exact fixed-size raw medium image and must
 * report BLOB_PRESENT.  The Foundation alone classifies publication from the
 * commit marker: an entirely erased marker means uncommitted regardless of
 * body bytes; a valid marker means committed; any other marker is torn and
 * faults the domain.  BLOB_EMPTY is valid only for LOAD_WITNESS and means the
 * Provider has no witness object; it is never a shortcut for an erased slot.
 * Providers must derive all results from durable media after restart and must
 * not depend on volatile sidecar state.
 * 中文：LOAD_SLOT 必须返回固定长度的原始介质镜像并报告 BLOB_PRESENT。
 * 是否发布只由 Foundation 根据槽尾提交标记判断：标记全为擦除值时，无论正文
 * 内容如何都属于未提交；合法标记表示已提交；其他值表示撕裂并使该域失败关闭。
 * BLOB_EMPTY 仅适用于 LOAD_WITNESS，表示 Provider 中不存在 witness 对象，
 * 不能作为“空槽”捷径。Provider 重启后的全部结果必须从持久介质重建，不得
 * 依赖易失 sidecar 状态。 */

typedef struct ucn_persist_io_completion {
    uint16_t struct_size;
    uint16_t api_version;
    uint64_t io_token;
    ucn_result_t result;
    uint32_t exact_bytes;
    uint8_t phase;
    uint8_t blob_state;
    uint8_t slot_index;
    uint8_t reserved_zero;
} ucn_persist_io_completion_t;

typedef struct ucn_persist_witness_view {
    uint16_t struct_size;
    uint16_t api_version;
    ucn_persist_domain_key_t domain;
    uint64_t highest_maybe_published_generation;
    uint8_t state;
    uint8_t reserved_zero[7];
} ucn_persist_witness_view_t;

typedef ucn_persist_io_start_t (*ucn_persist_begin_load_slot_fn)(
    void *context,
    ucn_persist_domain_key_t domain,
    uint8_t slot_index,
    uint8_t *output_buffer,
    size_t exact_bytes,
    uint64_t io_token,
    ucn_persist_io_completion_t *completion_out);
typedef ucn_persist_io_start_t (*ucn_persist_begin_write_inactive_fn)(
    void *context,
    ucn_persist_domain_key_t domain,
    uint8_t slot_index,
    const uint8_t *input_buffer,
    size_t exact_bytes,
    uint64_t io_token,
    ucn_persist_io_completion_t *completion_out);
typedef ucn_persist_io_start_t (*ucn_persist_begin_readback_fn)(
    void *context,
    ucn_persist_domain_key_t domain,
    uint8_t slot_index,
    uint8_t *output_buffer,
    size_t exact_bytes,
    uint64_t io_token,
    ucn_persist_io_completion_t *completion_out);
typedef ucn_persist_io_start_t (*ucn_persist_begin_publish_marker_fn)(
    void *context,
    ucn_persist_domain_key_t domain,
    uint8_t slot_index,
    const uint8_t marker[UCN_PERSIST_COMMIT_MARKER_BYTES],
    uint64_t io_token,
    ucn_persist_io_completion_t *completion_out);
typedef ucn_persist_io_start_t (*ucn_persist_begin_load_witness_fn)(
    void *context,
    ucn_persist_domain_key_t domain,
    ucn_persist_witness_view_t *witness_out,
    uint64_t io_token,
    ucn_persist_io_completion_t *completion_out);
typedef ucn_persist_io_start_t (*ucn_persist_begin_advance_witness_fn)(
    void *context,
    ucn_persist_domain_key_t domain,
    uint64_t expected_old,
    uint64_t exact_new,
    uint64_t io_token,
    ucn_persist_io_completion_t *completion_out);
typedef ucn_persist_io_start_t (*ucn_persist_poll_fn)(
    void *context,
    uint64_t io_token,
    ucn_persist_io_phase_t expected_phase,
    ucn_persist_io_completion_t *completion_out);

typedef struct ucn_persistence_provider_vtable {
    uint16_t struct_size;
    uint16_t api_version;
    ucn_persist_begin_load_slot_fn begin_load_slot;
    ucn_persist_begin_write_inactive_fn begin_write_inactive;
    ucn_persist_begin_readback_fn begin_readback;
    ucn_persist_begin_publish_marker_fn begin_publish_commit_marker;
    ucn_persist_begin_load_witness_fn begin_load_witness;
    ucn_persist_begin_advance_witness_fn begin_advance_witness;
    /* EN: Optional for a strictly synchronous Provider. If any begin_*()
     * returns PENDING while poll is NULL, the active durable domain faults.
     * 中文：严格同步 Provider 可不提供 poll；若 poll 为空但任一 begin_*()
     * 返回 PENDING，当前持久化域立即失败关闭。 */
    ucn_persist_poll_fn poll;
} ucn_persistence_provider_vtable_t;

typedef struct ucn_persistence_provider {
    uint16_t struct_size;
    uint16_t api_version;
    void *context;
    const ucn_persistence_provider_vtable_t *vtable;
    uint32_t minimum_write_alignment;
    uint32_t minimum_erase_alignment;
    uint32_t maximum_slot_bytes;
    uint16_t atomic_marker_bytes;
    uint8_t erased_value;
    uint8_t reserved_zero;
} ucn_persistence_provider_t;

/* EN: Volatile composition binding for one durable domain. It assigns the
 * only business Owner allowed to submit that domain during this boot and a
 * fresh generation used to reject stale continuations. It is deliberately
 * excluded from the durable Record and Manifest digest.
 * 中文：单个持久化域的易失装配绑定。它指定本次启动中唯一可提交该域的业务
 * Owner，并以新鲜代际拒绝旧 continuation；该对象不写入 Record，也不进入
 * Durable Manifest 摘要。 */
typedef struct ucn_persist_domain_binding {
    uint16_t struct_size;
    uint16_t api_version;
    ucn_persist_domain_key_t domain;
    uint16_t business_owner_instance;
    uint16_t domain_generation;
    uint32_t reserved_zero;
} ucn_persist_domain_binding_t;

typedef struct ucn_persistence_config {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t runtime_instance;
    uint16_t owner_instance;
    uint16_t reserved_zero;
    uint32_t required_domain_mask;
    const ucn_persist_manifest_t *manifest;
    const ucn_persist_domain_binding_t *domain_bindings;
    uint16_t domain_binding_count;
    uint16_t reserved_zero2;
    const ucn_persistence_provider_t *provider;
    /* EN: Blocking state lock. Normal contention must acquire the lock before
     * enter() returns UCN_OK; it must not be the shared callback gate lock.
     * Provider callbacks are always invoked after this lock is released.
     * 中文：可等待的状态锁。普通竞争必须在实际取得锁后才由 enter() 返回
     * UCN_OK；它不得与共享回调门使用同一把锁。Provider 回调前总会释放此锁。 */
    ucn_lock_ops_t state_lock;
    /* EN: Caller-owned, task/ISR/SMP-safe try gate shared by every Owner that
     * can enter the same Provider callback domain. It also owns the
     * domain-wide, non-wrapping I/O-token allocator, so Owners sharing a
     * Provider cannot issue the same token. Contention returns an immediate
     * error. 中文：调用方持有、任务/ISR/SMP 安全的 try gate，由同一 Provider
     * 回调域内所有 Owner 共享；它同时持有该域全局、不回绕的 I/O Token
     * 分配器，避免共享 Provider 的多个 Owner 产生相同 Token；竞争时立即报错。 */
    ucn_persist_callback_gate_t *shared_callback_gate;
    /* EN: Exclusive scratch for init-time Manifest verification. It must not
     * alias Owner, Provider, lock, Manifest, binding, or output storage.
     * 中文：初始化 Manifest 校验的独占临时区；不得与 Owner、Provider、锁、
     * Manifest、绑定或输出存储重叠。 */
    ucn_persistence_digest_workspace_t *digest_workspace;
} ucn_persistence_config_t;

size_t ucn_persistence_storage_required(void);
size_t ucn_persist_gate_storage_required(void);

ucn_result_t ucn_persist_callback_gate_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_lock_ops_t *lock,
    ucn_persist_callback_gate_t **gate_out);
/* EN: Deinitialization succeeds only after every attached Persistence Owner
 * has been deinitialized and no Provider callback is active.
 * 中文：只有所有引用该 Gate 的 Persistence Owner 都已反初始化且当前没有
 * Provider callback 时，Gate 反初始化才会成功。 */
ucn_result_t ucn_persist_callback_gate_deinit(
    ucn_persist_callback_gate_t *gate);

ucn_result_t ucn_persistence_manifest_digest(
    const ucn_persist_manifest_t *manifest,
    ucn_persistence_digest_workspace_t *workspace,
    uint8_t digest_out[UCN_PERSIST_DIGEST_BYTES]);

ucn_result_t ucn_persistence_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_persistence_config_t *config,
    ucn_persistence_owner_t **owner_out);
ucn_result_t ucn_persistence_deinit(ucn_persistence_owner_t *owner);

#ifdef __cplusplus
}
#endif

#endif
