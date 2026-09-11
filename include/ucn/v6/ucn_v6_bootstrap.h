#ifndef UCN_V6_BOOTSTRAP_H
#define UCN_V6_BOOTSTRAP_H

#include "ucn/v6/ucn_v6_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_BOOTSTRAP_MAX_PENDING \
    ((size_t)UCN_V6_CONFIG_BOOTSTRAP_PENDING)
#define UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS \
    ((size_t)UCN_V6_CONFIG_BOOTSTRAP_LINKS)

typedef enum ucn_v6_bootstrap_flow {
    UCN_V6_BOOTSTRAP_FLOW_JOIN = 1,
    UCN_V6_BOOTSTRAP_FLOW_REAUTH = 2
} ucn_v6_bootstrap_flow_t;

typedef enum ucn_v6_bootstrap_phase {
    UCN_V6_BOOTSTRAP_EMPTY = 0,
    UCN_V6_BOOTSTRAP_COOKIE_VERIFIED = 1,
    UCN_V6_BOOTSTRAP_AUTHORITY_VERIFIED = 2,
    UCN_V6_BOOTSTRAP_DEVICE_VERIFIED = 3,
    UCN_V6_BOOTSTRAP_ADDRESS_OFFERED = 4,
    UCN_V6_BOOTSTRAP_DEVICE_COMMITTED = 5,
    UCN_V6_BOOTSTRAP_FINAL_DURABLE = 6,
    UCN_V6_BOOTSTRAP_ABORTED = 7
} ucn_v6_bootstrap_phase_t;

typedef struct ucn_v6_bootstrap_key {
    uint16_t ingress_link_id;
    uint32_t ingress_link_generation;
    uint32_t local_peer_discriminator;
    ucn_v6_principal_t identity_digest;
    uint64_t transaction_id;
} ucn_v6_bootstrap_key_t;

typedef struct ucn_v6_bootstrap_transcript {
    uint8_t protocol_version;
    uint16_t bootstrap_header_contract;
    ucn_v6_bootstrap_flow_t flow;
    ucn_v6_principal_t joining_device_principal;
    ucn_v6_principal_t joining_device_identity_digest;
    ucn_v6_principal_t authority_principal;
    uint32_t authority_generation;
    uint64_t device_nonce;
    uint64_t authority_nonce;
    uint64_t transaction_id;
    uint64_t lease_freshness_challenge_nonce;
    uint32_t realm_id;
    uint32_t proposed_address;
    uint32_t address_binding_generation;
    uint32_t authority_address;
    uint32_t authority_binding_generation;
    uint16_t selected_link_instance_id;
    uint8_t binding_lease_id[16];
    uint64_t binding_lease_duration_us;
    uint64_t authority_lease_sequence;
    uint64_t authority_lease_duration_us;
    uint64_t freshness_max_remaining_lease_us;
    uint8_t durable_fence_token[16];
    uint8_t allocation_high_water_digest[16];
    uint8_t quorum_config_digest[UCN_V6_AUTHORITY_DIGEST_BYTES];
    uint8_t signer_set_digest[UCN_V6_AUTHORITY_DIGEST_BYTES];
    uint8_t threshold_proof_digest[UCN_V6_AUTHORITY_DIGEST_BYTES];
    uint8_t freshness_proof_transcript_hash[UCN_V6_AUTHORITY_DIGEST_BYTES];
    uint16_t authority_signer_count;
    uint16_t authority_quorum_threshold;
    uint8_t binding_mode;
    uint8_t selected_hop_suite;
    uint16_t selected_hop_key_id;
    uint32_t selected_hop_key_generation;
    uint8_t selected_e2e_mode;
    uint8_t selected_e2e_suite;
    uint16_t selected_e2e_key_id;
    uint32_t selected_e2e_key_generation;
    uint32_t selected_session_generation;
    uint32_t selected_link_instance_generation;
    uint8_t prior_messages_hash[32];
} ucn_v6_bootstrap_transcript_t;

typedef struct ucn_v6_bootstrap_pending {
    bool occupied;
    ucn_v6_bootstrap_flow_t flow;
    ucn_v6_bootstrap_phase_t phase;
    ucn_v6_bootstrap_key_t key;
    ucn_v6_bootstrap_transcript_t transcript;
    ucn_v6_binding_key_t existing_binding;
    /* EN: Local monotonic time captured by the Bootstrap Owner when the
     * authenticated Cookie opens this transaction.  Security lease admission
     * must derive from this immutable value, never from a later caller input.
     * 中文：认证 Cookie 打开事务时由 Bootstrap Owner 捕获的本地单调时间。
     * Security 租约准入必须由此不可变值派生，禁止由后续调用方指定。 */
    uint64_t challenge_started_local_us;
    uint64_t deadline_us;
} ucn_v6_bootstrap_pending_t;

typedef struct ucn_v6_bootstrap_link_budget {
    bool occupied;
    uint16_t ingress_link_id;
    uint32_t ingress_link_generation;
    uint8_t tokens;
    uint64_t last_refill_us;
    uint64_t last_activity_us;
} ucn_v6_bootstrap_link_budget_t;

typedef struct ucn_v6_bootstrap_config {
    uint8_t max_pending;
    uint8_t max_pending_per_link;
    uint8_t token_burst;
    uint8_t tokens_per_second;
    uint64_t pending_timeout_us;
} ucn_v6_bootstrap_config_t;

typedef struct ucn_v6_bootstrap_owner ucn_v6_bootstrap_owner_t;
#ifndef UCN_V6_BOOTSTRAP_OWNER_STORAGE_BYTES
#define UCN_V6_BOOTSTRAP_OWNER_STORAGE_BYTES                           \
    ((size_t)(512U + UCN_V6_CONFIG_BOOTSTRAP_PENDING * 2U * 512U +   \
              UCN_V6_CONFIG_BOOTSTRAP_LINKS * 48U))
#endif
typedef union ucn_v6_bootstrap_owner_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_BOOTSTRAP_OWNER_STORAGE_BYTES];
} ucn_v6_bootstrap_owner_storage_t;

struct ucn_v6_feature_manifest;

typedef enum ucn_v6_bootstrap_event {
    UCN_V6_BOOTSTRAP_EVENT_COOKIE = 1,
    UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF = 2,
    UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF = 3,
    UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER = 4,
    UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT = 5,
    UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE = 6,
    UCN_V6_BOOTSTRAP_EVENT_ABORT = 7
} ucn_v6_bootstrap_event_t;

#define UCN_V6_BOOTSTRAP_EVIDENCE_MAX_BYTES ((size_t)128U)
#define UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES ((size_t)379U)
#define UCN_V6_BOOTSTRAP_LOGICAL_MAX_BYTES                           \
    (UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES + (size_t)1U +     \
     UCN_V6_BOOTSTRAP_EVIDENCE_MAX_BYTES)
#define UCN_V6_BOOTSTRAP_FRAGMENT_HEADER_BYTES ((size_t)36U)
#define UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES ((size_t)128U)
#define UCN_V6_BOOTSTRAP_MAX_FRAGMENTS ((uint8_t)4U)
#define UCN_V6_BOOTSTRAP_HELLO_BYTES ((size_t)40U)
#define UCN_V6_BOOTSTRAP_COOKIE_CHALLENGE_BYTES ((size_t)40U)
#define UCN_V6_BOOTSTRAP_COOKIE_MAX_BYTES ((uint8_t)16U)
#define UCN_V6_BOOTSTRAP_HELLO_COOKIE_FIXED_BYTES ((size_t)82U)

typedef struct ucn_v6_bootstrap_hello {
    ucn_v6_bootstrap_flow_t flow;
    ucn_v6_principal_t identity_digest;
    uint64_t device_nonce;
    uint64_t transaction_id;
} ucn_v6_bootstrap_hello_t;

typedef struct ucn_v6_bootstrap_cookie_challenge {
    ucn_v6_bootstrap_flow_t flow;
    uint64_t transaction_id;
    uint32_t cookie_time_bucket;
    uint8_t cookie_length;
    uint8_t cookie[UCN_V6_BOOTSTRAP_COOKIE_MAX_BYTES];
} ucn_v6_bootstrap_cookie_challenge_t;

/* EN: Canonical, bounded proof bytes for exactly one Bootstrap event.  The
 * trusted verifier interprets them in the supplied event/transcript domain.
 * 中文：仅对应一个 Bootstrap 事件的规范、有界证明字节。可信验证器必须在
 * 给定 Event/Transcript 域内解释它。 */
typedef struct ucn_v6_bootstrap_evidence {
    uint16_t length;
    uint8_t bytes[UCN_V6_BOOTSTRAP_EVIDENCE_MAX_BYTES];
} ucn_v6_bootstrap_evidence_t;

typedef struct ucn_v6_bootstrap_hello_cookie {
    ucn_v6_bootstrap_flow_t flow;
    ucn_v6_principal_t identity_digest;
    uint64_t device_nonce;
    uint64_t transaction_id;
    uint64_t lease_freshness_challenge_nonce;
    uint16_t selected_link_instance_id;
    uint32_t selected_link_instance_generation;
    uint8_t prior_messages_hash[32];
    ucn_v6_bootstrap_evidence_t cookie_evidence;
} ucn_v6_bootstrap_hello_cookie_t;

/* EN: One strict post-Cookie Bootstrap fragment.  The outer Opcode selects
 * the event; this header repeats the immutable transaction key so fragment
 * zero can be admitted only against an already-open Bootstrap pending slot.
 * Fragments are canonical, contiguous and fixed at 128 bytes except the last.
 * 中文：Cookie 之后的严格 Bootstrap 分片。外层 Opcode 选择事件；本头重复
 * 不可变事务键，使第 0 片只能匹配已打开的 Bootstrap pending 后才被接纳。
 * 分片必须规范、连续，除最后一片外固定承载 128 字节。 */
typedef struct ucn_v6_bootstrap_fragment {
    ucn_v6_bootstrap_flow_t flow;
    ucn_v6_bootstrap_phase_t phase;
    uint8_t fragment_index;
    uint8_t fragment_count;
    uint16_t total_length;
    uint16_t fragment_offset;
    uint16_t fragment_length;
    uint64_t transaction_id;
    ucn_v6_principal_t identity_digest;
    uint8_t data[UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES];
} ucn_v6_bootstrap_fragment_t;

typedef struct ucn_v6_bootstrap_reassembly {
    bool occupied;
    uint16_t protocol_opcode;
    ucn_v6_bootstrap_phase_t phase;
    ucn_v6_bootstrap_key_t key;
    uint16_t total_length;
    uint8_t fragment_count;
    uint8_t received_mask;
    uint64_t deadline_us;
    uint8_t logical[UCN_V6_BOOTSTRAP_LOGICAL_MAX_BYTES];
} ucn_v6_bootstrap_reassembly_t;

/* EN: Exact pre-state Bootstrap payloads. HELLO and COOKIE_CHALLENGE are
 * fixed 40-byte records, so the stateless response never amplifies the
 * request. HELLO_COOKIE is a single bounded record; its Cookie is verified
 * before any fragment-reassembly or Bootstrap pending state is allocated.
 * 中文：精确的认证前 Bootstrap Payload。HELLO 与 COOKIE_CHALLENGE 固定为
 * 40 字节，确保无状态响应不放大请求。HELLO_COOKIE 是单个有界记录；其
 * Cookie 必须在分片重组或 Bootstrap pending 分配前完成验证。 */
ucn_v6_result_t ucn_v6_bootstrap_hello_encode(
    const ucn_v6_bootstrap_hello_t *hello,
    uint8_t output[UCN_V6_BOOTSTRAP_HELLO_BYTES]);
ucn_v6_result_t ucn_v6_bootstrap_hello_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_bootstrap_hello_t *hello);
ucn_v6_result_t ucn_v6_bootstrap_cookie_challenge_encode(
    const ucn_v6_bootstrap_cookie_challenge_t *challenge,
    uint8_t output[UCN_V6_BOOTSTRAP_COOKIE_CHALLENGE_BYTES]);
ucn_v6_result_t ucn_v6_bootstrap_cookie_challenge_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_bootstrap_cookie_challenge_t *challenge);
ucn_v6_result_t ucn_v6_bootstrap_hello_cookie_encode(
    const ucn_v6_bootstrap_hello_cookie_t *hello_cookie,
    uint8_t *output, size_t output_capacity, size_t *output_length);
ucn_v6_result_t ucn_v6_bootstrap_hello_cookie_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_bootstrap_hello_cookie_t *hello_cookie);

/* EN: Canonical ordered Bootstrap transcript codec. The expected phase is
 * external state owned by the bounded FSM; it selects which later field
 * groups must already be present. Earlier messages encode future fields as
 * canonical zero. Decode/encode rejection never writes the output object.
 * 中文：Bootstrap 有序 Transcript 的唯一规范 Codec。Expected phase 属于
 * 有界 FSM 的外部状态，用来决定哪些后续字段组必须已存在；早期消息把未来
 * 字段规范编码为零。编解码拒绝时绝不写回输出对象。 */
ucn_v6_result_t ucn_v6_bootstrap_transcript_encode(
    const ucn_v6_bootstrap_transcript_t *transcript,
    ucn_v6_bootstrap_phase_t expected_phase,
    uint8_t output[UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES]);
ucn_v6_result_t ucn_v6_bootstrap_transcript_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_bootstrap_phase_t expected_phase,
    ucn_v6_bootstrap_transcript_t *transcript);

/* EN: Maps exactly one post-Cookie event to its frozen Bootstrap Opcode.
 * Zero means the event is not a Wire message.
 * 中文：把一个 Cookie 后事件映射到冻结的 Bootstrap Opcode；0 表示该事件
 * 不是线上消息。 */
uint16_t ucn_v6_bootstrap_event_opcode(ucn_v6_bootstrap_event_t event);

/* EN: Builds/parses the canonical logical event body:
 * transcript[379] + evidence_length:u8 + evidence.  The expected phase is
 * explicit so an earlier-stage transcript cannot be interpreted as a later
 * proof. Rejection never writes output.
 * 中文：构建/解析规范逻辑事件体：379 字节 transcript + 1 字节证明长度 +
 * proof。显式 expected phase 防止早期 transcript 被解释成后期证明；拒绝
 * 时不写回输出。 */
ucn_v6_result_t ucn_v6_bootstrap_logical_encode(
    ucn_v6_bootstrap_event_t event,
    ucn_v6_bootstrap_phase_t expected_phase,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_bootstrap_evidence_t *evidence,
    uint8_t *output, size_t output_capacity, size_t *output_length);
ucn_v6_result_t ucn_v6_bootstrap_logical_decode(
    uint16_t protocol_opcode,
    ucn_v6_bootstrap_phase_t expected_phase,
    const uint8_t *input, size_t input_length,
    ucn_v6_bootstrap_transcript_t *transcript,
    ucn_v6_bootstrap_evidence_t *evidence);

/* EN: Encodes/decodes one exact fragment payload.  The encoded header is
 * always 36 bytes; total length and offsets must describe the unique
 * contiguous partition of the logical body.
 * 中文：编解码一个精确分片 Payload。编码头固定 36 字节；总长与偏移必须
 * 描述逻辑消息唯一的连续分区。 */
ucn_v6_result_t ucn_v6_bootstrap_fragment_encode(
    const ucn_v6_bootstrap_fragment_t *fragment,
    uint8_t *output, size_t output_capacity, size_t *output_length);
ucn_v6_result_t ucn_v6_bootstrap_fragment_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_bootstrap_fragment_t *fragment);

/* EN: Accepts one decoded fragment only after the caller has resolved the
 * exact Bootstrap pending key and immutable pending deadline. A different
 * key never evicts or expires the occupied object; expiry/reset is explicit.
 * Duplicate fragments are accepted only when byte-identical.
 * 中文：调用方解析到精确 Bootstrap pending key 与不可变截止期后，才接纳
 * 一个已解码分片。不同 key 绝不能驱逐或借机过期现有对象；过期/复位必须
 * 显式执行。重复分片仅在字节完全相同时幂等接受。 */
ucn_v6_result_t ucn_v6_bootstrap_reassembly_accept(
    ucn_v6_bootstrap_reassembly_t *reassembly,
    uint16_t protocol_opcode,
    const ucn_v6_bootstrap_key_t *key,
    uint64_t pending_deadline_us,
    uint64_t now_us,
    const ucn_v6_bootstrap_fragment_t *fragment,
    bool *complete);
ucn_v6_result_t ucn_v6_bootstrap_reassembly_borrow(
    const ucn_v6_bootstrap_reassembly_t *reassembly,
    const uint8_t **logical, size_t *logical_length);
ucn_v6_result_t ucn_v6_bootstrap_reassembly_reset(
    ucn_v6_bootstrap_reassembly_t *reassembly);

typedef struct ucn_v6_bootstrap_verifier_ops {
    void *context;
    ucn_v6_result_t (*authorize_event)(
        void *context,
        ucn_v6_bootstrap_event_t event,
        ucn_v6_bootstrap_flow_t flow,
        const ucn_v6_bootstrap_key_t *key,
        const ucn_v6_bootstrap_transcript_t *transcript,
        const ucn_v6_binding_key_t *existing_binding,
        uint64_t now_us,
        const ucn_v6_bootstrap_evidence_t *evidence);
} ucn_v6_bootstrap_verifier_ops_t;

/* EN: Initializes bounded JOIN and REAUTH state owned by one protocol task.
 * 中文：初始化由单一协议任务持有的有界 JOIN/REAUTH 状态。 */
ucn_v6_result_t ucn_v6_bootstrap_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const struct ucn_v6_feature_manifest *manifest,
    const ucn_v6_bootstrap_config_t *config,
    const ucn_v6_bootstrap_verifier_ops_t *verifier,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_bootstrap_owner_t **owner);
/* EN: Applies no-amplification and the exact {Link ID, generation} budget.
 * 中文：执行无放大与精确 {Link ID, generation} 认证前令牌预算。 */
ucn_v6_result_t ucn_v6_bootstrap_admit_initial_hello(
    ucn_v6_bootstrap_owner_t *owner,
    uint16_t ingress_link_id,
    uint32_t ingress_link_generation,
    uint64_t now_us,
    size_t request_bytes,
    size_t response_bytes);
/* EN: Opens a fixed pending slot only after a stateless Cookie succeeds and
 * only when the key's exact ingress {Link ID,generation} equals the selected
 * Link instance committed by the transcript.
 * 中文：仅在无状态 Cookie 成功，且 Key 的精确入站 {Link ID,代际} 与
 * Transcript 承诺的 Selected Link Instance 完全一致后创建固定 pending 槽。 */
ucn_v6_result_t ucn_v6_bootstrap_open_after_cookie(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_binding_key_t *existing_binding,
    const ucn_v6_bootstrap_evidence_t *cookie_evidence,
    uint64_t now_us);
/* EN: Advances the mutually authenticated transcript in strict order.
 * 中文：按严格顺序推进双向认证 transcript。 */
ucn_v6_result_t ucn_v6_bootstrap_advance(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    ucn_v6_bootstrap_event_t event,
    const ucn_v6_bootstrap_evidence_t *evidence,
    uint64_t now_us);
/* EN: Copies diagnostics without exposing a writable internal slot.
 * 中文：复制诊断快照，不暴露可写的内部槽位。 */
ucn_v6_result_t ucn_v6_bootstrap_copy_pending(
    const ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    ucn_v6_bootstrap_pending_t *pending);
/* EN: Proves that the unique bounded FSM reached its exact durable final
 * state before Security may install ADMITTED state.
 * 中文：证明唯一有界 FSM 已到达精确的持久终态，Security 才可安装
 * ADMITTED 状态。 */
ucn_v6_result_t ucn_v6_bootstrap_validate_final(
    const ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    uint64_t now_us);
/* EN: Explicitly expires half-open deadlines and idle Link-generation rate
 * slots. A rate slot is reclaimed only when no pending transaction refers to
 * it and its idle interval has elapsed; hostile input never evicts live state.
 * The return value counts expired pending transactions, not rate slots.
 * 中文：显式清理半开截止期和空闲 Link-generation 限流槽。
 * 仅当没有 pending 事务引用且空闲期已到时才回收限流槽；恶意
 * 输入不能驱逐活跃状态。返回值只统计过期 pending 事务数。 */
size_t ucn_v6_bootstrap_expire(
    ucn_v6_bootstrap_owner_t *owner,
    uint64_t now_us);

/* EN: Atomically removes every half-open JOIN/REAUTH transaction bound to
 * one exact retired Link instance.  A Link ID without its generation is not
 * sufficient to invalidate Bootstrap state.
 * 中文：原子清除绑定到一个精确已退休 Link 实例的全部半开 JOIN/REAUTH
 * 事务；只有 Link ID 而没有代际不足以失效 Bootstrap 状态。 */
size_t ucn_v6_bootstrap_invalidate_link(
    ucn_v6_bootstrap_owner_t *owner,
    uint16_t link_id,
    uint32_t link_generation);

#ifdef __cplusplus
}
#endif

#endif
