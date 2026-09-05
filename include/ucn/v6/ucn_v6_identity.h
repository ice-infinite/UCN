#ifndef UCN_V6_IDENTITY_H
#define UCN_V6_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ucn/v6/ucn_v6_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_PROTOCOL_VERSION ((uint8_t)6U)
#define UCN_V6_SERIAL_ROTATION_THRESHOLD UINT32_C(0xFFFFFFFE)
#define UCN_V6_SERIAL64_ROTATION_THRESHOLD UINT64_C(0xFFFFFFFFFFFFFFFE)
#define UCN_V6_MAX_BINDING_SLOTS ((size_t)UCN_V6_CONFIG_MAX_BINDINGS)
#define UCN_V6_MAX_ACTIVE_GROUPS ((size_t)UCN_V6_CONFIG_MAX_ACTIVE_GROUPS)
#define UCN_V6_AUTHORITY_DIGEST_BYTES ((size_t)32U)

#define UCN_V6_DURABLE_WITNESS_MAGIC UINT32_C(0x56365754)
#define UCN_V6_DURABLE_WITNESS_SCHEMA UINT16_C(1)
#define UCN_V6_DURABLE_WITNESS_COMMISSIONED UINT16_C(0x0001)

typedef enum ucn_v6_durable_witness_domain {
    UCN_V6_DURABLE_WITNESS_IDENTITY = 1,
    UCN_V6_DURABLE_WITNESS_SECURITY = 2
} ucn_v6_durable_witness_domain_t;

/* EN: Independent two-phase anti-rollback witness. Once commissioned, a
 * Provider must never return NOT_FOUND. pending_generation names the only
 * snapshot that may be recovered or atomically abandoned after a torn write.
 * 中文：独立的两阶段防回退 witness。完成 commissioned 后 Provider 永远不得
 * 返回 NOT_FOUND；pending_generation 指明撕裂写后唯一可恢复或原子放弃的快照。 */
typedef struct ucn_v6_durable_generation_witness {
    uint32_t magic;
    uint16_t schema;
    uint16_t flags;
    uint8_t domain;
    uint8_t reserved[7];
    uint64_t witness_generation;
    uint64_t committed_generation;
    uint64_t pending_generation;
} ucn_v6_durable_generation_witness_t;

typedef struct ucn_v6_principal {
    uint8_t bytes[16];
} ucn_v6_principal_t;

typedef struct ucn_v6_binding_key {
    uint32_t realm_id;
    uint32_t node_address;
    uint32_t binding_generation;
} ucn_v6_binding_key_t;

typedef struct ucn_v6_session_key {
    ucn_v6_binding_key_t binding;
    ucn_v6_principal_t principal;
    uint32_t session_generation;
} ucn_v6_session_key_t;

typedef enum ucn_v6_address_mode {
    UCN_V6_ADDRESS_STATIC = 1,
    UCN_V6_ADDRESS_LEASED = 2,
    UCN_V6_ADDRESS_SELF_PROPOSED = 3
} ucn_v6_address_mode_t;

typedef struct ucn_v6_authority_epoch {
    uint32_t realm_id;
    ucn_v6_principal_t authority_principal;
    uint32_t authority_generation;
    uint8_t durable_fence_token[16];
    uint64_t lease_sequence;
    uint64_t lease_duration_us;
    uint8_t allocation_high_water_digest[16];
    uint8_t quorum_config_digest[UCN_V6_AUTHORITY_DIGEST_BYTES];
    uint8_t signer_set_digest[UCN_V6_AUTHORITY_DIGEST_BYTES];
    uint8_t threshold_proof_digest[UCN_V6_AUTHORITY_DIGEST_BYTES];
    uint16_t signer_count;
    uint16_t quorum_threshold;
} ucn_v6_authority_epoch_t;

/* EN: Authenticated verifier-specific freshness statement. It never carries
 * a foreign absolute deadline; the receiver derives its local deadline from
 * challenge_started_local_us and its timer policy.
 * 中文：面向特定验证者的认证新鲜度声明。它不携带对端绝对截止期；接收端
 * 必须用本地 Challenge 起点与计时器策略推导自己的截止期。 */
typedef struct ucn_v6_authority_freshness {
    ucn_v6_principal_t verifier_device_principal;
    uint64_t challenge_nonce;
    uint64_t transaction_id;
    uint64_t authority_lease_sequence;
    uint64_t max_remaining_lease_us;
    uint8_t binding_lease_id[16];
    uint32_t binding_generation;
    uint8_t proof_transcript_hash[UCN_V6_AUTHORITY_DIGEST_BYTES];
} ucn_v6_authority_freshness_t;

typedef struct ucn_v6_lease_verifier_policy {
    uint32_t local_timer_max_slow_ppm;
    uint64_t local_timer_resolution_us;
    uint64_t local_timer_read_uncertainty_us;
    bool timer_read_uncertainty_known;
    uint64_t local_policy_max_lease_us;
} ucn_v6_lease_verifier_policy_t;

typedef enum ucn_v6_authority_transition_kind {
    UCN_V6_AUTHORITY_TRANSITION_INITIAL = 1,
    UCN_V6_AUTHORITY_TRANSITION_FRESHNESS = 2,
    UCN_V6_AUTHORITY_TRANSITION_RENEWAL = 3,
    UCN_V6_AUTHORITY_TRANSITION_TRANSFER = 4
} ucn_v6_authority_transition_kind_t;

/* EN: Exact, fieldwise Authority transition transcript presented to the
 * trusted verifier.  A factory transition has committed_epoch_valid=false
 * and a canonical-zero committed_epoch.  derived_local_deadline_us is the
 * receiver's checked, half-open local deadline; it is never supplied by the
 * untrusted caller.
 * 中文：提交给可信验证器的精确、逐字段 Authority 转换 transcript。工厂
 * 转换的 committed_epoch_valid=false，且 committed_epoch 必须规范清零。
 * derived_local_deadline_us 是接收端经检查得到的半开本地截止期，绝不由
 * 不可信调用者直接提供。 */
typedef struct ucn_v6_authority_transition_request {
    ucn_v6_authority_transition_kind_t kind;
    uint32_t realm_id;
    bool committed_epoch_valid;
    ucn_v6_authority_epoch_t committed_epoch;
    ucn_v6_authority_epoch_t proposed_epoch;
    ucn_v6_authority_freshness_t freshness;
    uint64_t challenge_started_local_us;
    ucn_v6_lease_verifier_policy_t lease_policy;
    uint64_t derived_local_deadline_us;
} ucn_v6_authority_transition_request_t;

#define UCN_V6_AUTHORITY_PROOF_MAX_BYTES ((size_t)128U)
#define UCN_V6_AUTHORITY_TRANSITION_CANONICAL_VERSION ((uint8_t)1U)
#define UCN_V6_AUTHORITY_TRANSITION_CANONICAL_BYTES ((size_t)497U)

/* EN: Bounded proof bytes for exactly one Authority transition transcript.
 * The Identity owner never interprets these bytes as a boolean assertion;
 * only its init-bound verifier may authorize them.
 * 中文：精确绑定单个 Authority 转换 transcript 的有界证明字节。Identity
 * Owner 绝不会把这些字节当作布尔断言；只有初始化时绑定的验证器可授权。 */
typedef struct ucn_v6_authority_proof {
    uint16_t length;
    uint8_t bytes[UCN_V6_AUTHORITY_PROOF_MAX_BYTES];
} ucn_v6_authority_proof_t;

typedef struct ucn_v6_identity_authority_verifier_ops {
    void *context;
    ucn_v6_result_t (*verify_epoch_transition)(
        void *context,
        const ucn_v6_authority_transition_request_t *request,
        const uint8_t *canonical_transition,
        size_t canonical_transition_bytes,
        const ucn_v6_authority_proof_t *proof);
} ucn_v6_identity_authority_verifier_ops_t;

typedef struct ucn_v6_binding_certificate {
    ucn_v6_principal_t device_principal;
    ucn_v6_principal_t authority_principal;
    ucn_v6_binding_key_t binding;
    uint32_t authority_generation;
    uint8_t lease_id[16];
    uint64_t lease_duration_us;
    uint64_t authority_lease_sequence;
    ucn_v6_address_mode_t mode;
} ucn_v6_binding_certificate_t;

typedef struct ucn_v6_binding_slot {
    bool occupied;
    bool active;
    uint32_t node_address;
    uint32_t generation_high_water;
    ucn_v6_binding_certificate_t certificate;
} ucn_v6_binding_slot_t;

typedef struct ucn_v6_group_allocator {
    uint32_t dynamic_group_id_high_water;
    uint32_t active_group_ids[UCN_V6_MAX_ACTIVE_GROUPS];
} ucn_v6_group_allocator_t;

#define UCN_V6_IDENTITY_SNAPSHOT_MAGIC UINT32_C(0x56364953)
#define UCN_V6_IDENTITY_SNAPSHOT_SCHEMA UINT16_C(1)

/* EN: Semantic durable snapshot of one Realm Address Authority. Providers
 * must serialize fields rather than treating compiler padding as persistent
 * meaning. Local monotonic lease deadlines are deliberately excluded and
 * must be re-established from a fresh authenticated challenge after restart.
 * 中文：一个 Realm 地址权威的语义持久快照。Provider 必须按字段序列化，
 * 不得把编译器 padding 当成持久语义。本地单调时钟租约截止期刻意不持久化，
 * 重启后必须通过新的认证租约挑战重新建立。 */
typedef struct ucn_v6_identity_snapshot {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t record_generation;
    uint32_t realm_id;
    bool epoch_valid;
    ucn_v6_authority_epoch_t epoch;
    ucn_v6_binding_slot_t bindings[UCN_V6_MAX_BINDING_SLOTS];
    ucn_v6_group_allocator_t groups;
} ucn_v6_identity_snapshot_t;

typedef struct ucn_v6_callback_gate {
    void *context;
    void (*lock)(void *context);
    void (*unlock)(void *context);
    const void *active_owner;
    uint64_t violation_count;
    bool initialized;
    bool active;
} ucn_v6_callback_gate_t;
#define UCN_V6_CALLBACK_GATE_INITIALIZER {0}

typedef struct ucn_v6_identity_store_ops {
    void *context;
    ucn_v6_result_t (*load_witness)(
        void *context,
        ucn_v6_durable_generation_witness_t *witness);
    ucn_v6_result_t (*reserve_witness)(
        void *context,
        const ucn_v6_durable_generation_witness_t *witness);
    ucn_v6_result_t (*load)(
        void *context,
        ucn_v6_identity_snapshot_t *snapshot);
    ucn_v6_result_t (*submit)(
        void *context,
        const ucn_v6_identity_snapshot_t *snapshot);
} ucn_v6_identity_store_ops_t;

typedef struct ucn_v6_identity_authority ucn_v6_identity_authority_t;
#ifndef UCN_V6_IDENTITY_AUTHORITY_STORAGE_BYTES
#define UCN_V6_IDENTITY_AUTHORITY_STORAGE_BYTES                       \
    ((size_t)(512U + UCN_V6_CONFIG_MAX_BINDINGS * 128U +             \
              UCN_V6_CONFIG_MAX_ACTIVE_GROUPS * 8U))
#endif
typedef union ucn_v6_identity_authority_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_IDENTITY_AUTHORITY_STORAGE_BYTES];
} ucn_v6_identity_authority_storage_t;

typedef struct ucn_v6_identity_authority_view {
    uint64_t record_generation;
    uint32_t realm_id;
    ucn_v6_authority_epoch_t epoch;
    uint64_t local_lease_deadline_us;
    uint32_t dynamic_group_id_high_water;
    uint16_t occupied_bindings;
    bool epoch_valid;
    bool faulted;
} ucn_v6_identity_authority_view_t;

struct ucn_v6_feature_manifest;

/* EN: Validates canonical identity and binding keys.
 * 中文：校验规范的身份与地址绑定键。 */
bool ucn_v6_principal_is_valid(const ucn_v6_principal_t *principal);
bool ucn_v6_binding_key_is_valid(const ucn_v6_binding_key_t *binding);
bool ucn_v6_binding_key_equal(
    const ucn_v6_binding_key_t *left,
    const ucn_v6_binding_key_t *right);
/* EN: Advances a no-wrap ownership serial or reports exhaustion.
 * 中文：推进不可回绕的所有权序列，耗尽时明确报错。 */
ucn_v6_result_t ucn_v6_serial_checked_next(
    uint32_t current,
    uint32_t *next);
/* EN: Builds and checks one conservative local half-open lease deadline.
 * 中文：建立并检查保守的本地半开租约截止期。 */
ucn_v6_result_t ucn_v6_lease_deadline_build(
    uint64_t challenge_started_local_us,
    uint64_t max_remaining_lease_us,
    const ucn_v6_lease_verifier_policy_t *policy,
    uint64_t *local_deadline_us);
bool ucn_v6_lease_deadline_is_live(uint64_t now_us, uint64_t deadline_us);

/* EN: Encodes the frozen, padding-independent Authority proof domain.  The
 * exact big-endian order is: protocol version, canonical contract version,
 * transition kind, committed-valid flag, Realm, committed Epoch, proposed
 * Epoch, freshness statement, challenge start, lease policy, and derived
 * local deadline.  The committed Epoch occupies its fixed zero-filled field
 * for a factory transition.
 * 中文：编码冻结且不依赖 padding 的 Authority 证明域。精确大端顺序为：
 * 协议版本、规范合同版本、转换类型、committed-valid、Realm、旧 Epoch、
 * 新 Epoch、Freshness、Challenge 起点、租约策略及推导出的本地截止期。
 * 工厂转换仍保留固定长度、全零的旧 Epoch 字段。 */
ucn_v6_result_t ucn_v6_authority_transition_encode_canonical(
    const ucn_v6_authority_transition_request_t *request,
    uint8_t *output,
    size_t output_bytes,
    size_t *written_bytes);

/* EN: Initializes a caller-owned, shared Provider callback fence. Every
 * semantic field must start zero/null (normally via the initializer macro);
 * object padding is ignored. An initialized gate cannot be reset here.
 * 中文：初始化由调用方持有、跨对象共享的 Provider 回调围栏。所有语义字段
 * 必须从零/null 开始（通常使用初始化宏）；对象 padding 被忽略。已初始化围栏
 * 不能通过本接口重置。 */
ucn_v6_result_t ucn_v6_callback_gate_init(
    ucn_v6_callback_gate_t *gate,
    void *context,
    void (*lock)(void *context),
    void (*unlock)(void *context));
/* EN: Reads the shared callback fence under its caller-supplied lock.
 * 中文：在调用方提供的锁保护下读取共享回调围栏。 */
bool ucn_v6_callback_gate_is_active(ucn_v6_callback_gate_t *gate);
/* EN: Returns the monotonic count of rejected nested/concurrent entries.
 * Providers cannot hide a re-entry attempt by discarding the inner error.
 * 中文：返回被拒绝的嵌套/并发进入次数。Provider 即使丢弃内层错误，
 * 外层调用也能发现重入尝试并失败关闭。 */
uint64_t ucn_v6_callback_gate_violation_count(
    ucn_v6_callback_gate_t *gate);
/* EN: Enters or leaves the shared Provider callback dynamic extent.
 * 中文：进入或退出共享 Provider 回调动态范围。 */
ucn_v6_result_t ucn_v6_callback_gate_try_enter(
    ucn_v6_callback_gate_t *gate,
    const void *owner);
ucn_v6_result_t ucn_v6_callback_gate_leave(
    ucn_v6_callback_gate_t *gate,
    const void *owner);

/* EN: Initializes the isolated Realm Address Authority model.
 * 中文：初始化隔离的 Realm 地址权威模型。 */
ucn_v6_result_t ucn_v6_identity_authority_init_in_place(
    void *storage,
    size_t storage_bytes,
    const struct ucn_v6_feature_manifest *manifest,
    uint32_t realm_id,
    const ucn_v6_identity_authority_verifier_ops_t *verifier,
    const ucn_v6_identity_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_identity_authority_t **authority);
/* EN: Persists and publishes an exact Authority transition. A factory Realm
 * starts at Authority Generation 1 / Lease Sequence 1; later transitions are
 * idempotent freshness, checked-next lease renewal, or checked-next fenced
 * transfer. Existing Binding Certificates retain their exact issuing Epoch
 * and never inherit the new lease implicitly.
 * 中文：持久化并发布精确的 Authority 转换。新 Realm 必须从 Authority
 * Generation 1 / Lease Sequence 1 开始；后续仅允许幂等新鲜度、
 * checked-next 租约续期，或 checked-next 且带新 Fence 的权威换主。
 * 既有 Binding Certificate 保留其精确签发 Epoch，绝不隐式继承新租约。 */
ucn_v6_result_t ucn_v6_identity_authority_install_epoch(
    ucn_v6_identity_authority_t *authority,
    const ucn_v6_authority_epoch_t *epoch,
    const ucn_v6_authority_freshness_t *freshness,
    uint64_t challenge_started_local_us,
    const ucn_v6_lease_verifier_policy_t *lease_policy,
    const ucn_v6_authority_proof_t *proof);
/* EN: Allocates or retires Binding ownership with persist-before-publish.
 * 中文：按先持久化后发布原则分配或退休 Binding 所有权。 */
ucn_v6_result_t ucn_v6_identity_authority_allocate_binding(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t node_address,
    const ucn_v6_principal_t *device_principal,
    ucn_v6_address_mode_t mode,
    const uint8_t lease_id[16],
    uint64_t lease_duration_us,
    ucn_v6_binding_certificate_t *certificate);
/* EN: Persistently re-endorses one still-active Binding under the current
 * Authority Epoch without changing its Binding Generation.  This is the only
 * legal bridge from an historical issuer to a post-transfer REAUTH
 * transcript; a second, different endorsement in the same Authority Epoch is
 * rejected as replay ambiguity.
 * 中文：在不改变 Binding Generation 的前提下，用当前 Authority Epoch 对
 * 仍有效 Binding 做持久重签。这是历史签发者过渡到换主后 REAUTH transcript
 * 的唯一合法桥；同一 Authority Epoch 下不同的第二份背书会因重放歧义被拒。 */
ucn_v6_result_t ucn_v6_identity_authority_reendorse_binding(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    const ucn_v6_binding_key_t *binding,
    const ucn_v6_principal_t *device_principal,
    ucn_v6_address_mode_t mode,
    const uint8_t lease_id[16],
    uint64_t lease_duration_us,
    ucn_v6_binding_certificate_t *certificate);
ucn_v6_result_t ucn_v6_identity_authority_retire_binding(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t node_address,
    uint32_t binding_generation);
/* EN: Allocates or retires dynamic Group IDs without reusing holes.
 * 中文：不复用历史空洞地分配或退休动态 Group ID。 */
ucn_v6_result_t ucn_v6_identity_authority_allocate_dynamic_group(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t *group_id);
ucn_v6_result_t ucn_v6_identity_authority_retire_dynamic_group(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t group_id);
/* EN: Copies a read-only diagnostic view after validating opaque storage.
 * 中文：校验 opaque storage 后复制只读诊断视图。 */
ucn_v6_result_t ucn_v6_identity_authority_copy_view(
    const ucn_v6_identity_authority_t *authority,
    ucn_v6_identity_authority_view_t *view);

#ifdef __cplusplus
}
#endif

#endif
