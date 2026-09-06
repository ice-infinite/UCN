#ifndef UCN_V6_SECURITY_H
#define UCN_V6_SECURITY_H

#include "ucn/v6/ucn_v6_bootstrap.h"
#include "ucn/v6/ucn_v6_config.h"
#include "ucn/v6/ucn_v6_owner.h"
#include "ucn/v6/ucn_v6_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_SECURITY_PROOF_MAX_BYTES ((size_t)96U)
#define UCN_V6_SECURITY_REPLAY_BITS ((uint8_t)64U)
#define UCN_V6_SECURITY_SNAPSHOT_MAGIC UINT32_C(0x56533653)
#define UCN_V6_SECURITY_SNAPSHOT_SCHEMA UINT16_C(3)
#define UCN_V6_SECURITY_JOIN_RECEIPT_BYTES ((size_t)388U)
#define UCN_V6_SECURITY_INVALIDATION_DEPTH                              \
    ((size_t)UCN_V6_CONFIG_SECURITY_SESSIONS)

#define UCN_V6_SUITE_HMAC_SHA256_128 ((uint8_t)1U)
#define UCN_V6_SUITE_AES_GCM_128 ((uint8_t)2U)
#define UCN_V6_SUITE_CHACHA20_POLY1305 ((uint8_t)3U)

typedef enum ucn_v6_security_direction {
    UCN_V6_SECURITY_INBOUND = 1,
    UCN_V6_SECURITY_OUTBOUND = 2
} ucn_v6_security_direction_t;

typedef enum ucn_v6_operation_id_policy {
    UCN_V6_OPERATION_ID_NONE = 1,
    UCN_V6_OPERATION_ID_ANY_NONZERO = 2,
    UCN_V6_OPERATION_ID_EXACT = 3
} ucn_v6_operation_id_policy_t;

typedef enum ucn_v6_security_proof_role {
    UCN_V6_PROOF_JOINING_DEVICE = 1,
    UCN_V6_PROOF_ADDRESS_AUTHORITY = 2,
    UCN_V6_PROOF_REALM_ADMIN = 3,
    UCN_V6_PROOF_SESSION_DURABLE_RECEIPT = 4
} ucn_v6_security_proof_role_t;

typedef enum ucn_v6_group_slot_state {
    UCN_V6_GROUP_SLOT_NEVER_ACTIVATED = 0,
    UCN_V6_GROUP_SLOT_ACTIVE = 1,
    UCN_V6_GROUP_SLOT_RETIRED = 2
} ucn_v6_group_slot_state_t;

typedef enum ucn_v6_group_key_state {
    UCN_V6_GROUP_KEY_NEVER_ACTIVATED = 0,
    UCN_V6_GROUP_KEY_ACTIVE = 1,
    UCN_V6_GROUP_KEY_RETIRED = 2
} ucn_v6_group_key_state_t;

typedef struct ucn_v6_key_selector {
    uint8_t suite_id;
    uint16_t key_id;
    uint32_t key_generation;
} ucn_v6_key_selector_t;

typedef struct ucn_v6_replay_window {
    uint32_t highest_sequence;
    uint64_t seen_bitmap;
    bool initialized;
} ucn_v6_replay_window_t;

typedef struct ucn_v6_security_session_record {
    bool occupied;
    bool admitted;
    bool revoked;
    bool requires_reauth;
    ucn_v6_principal_t peer_principal;
    ucn_v6_binding_key_t local_binding;
    ucn_v6_binding_key_t peer_binding;
    uint32_t session_generation;
    uint16_t link_instance_id;
    uint32_t link_instance_generation;
    ucn_v6_bootstrap_flow_t bootstrap_flow;
    uint64_t bootstrap_transaction_id;
    uint64_t bootstrap_device_nonce;
    uint64_t bootstrap_authority_nonce;
    uint64_t bootstrap_freshness_nonce;
    uint8_t bootstrap_prior_messages_hash[32];
    /* EN: Exact canonical JOIN/REAUTH transcript that produced this durable
     * session.  Keeping the complete transcript makes a durable receipt an
     * export of committed state rather than a caller-supplied assertion.
     * 中文：产生该持久 Session 的完整规范 JOIN/REAUTH transcript。保留完整
     * transcript，使持久回执只能导出已提交状态，不能由调用方任意声明。 */
    ucn_v6_bootstrap_transcript_t bootstrap_transcript;
    ucn_v6_e2e_mode_t e2e_mode;
    ucn_v6_authority_epoch_t authority_epoch;
    ucn_v6_authority_freshness_t authority_freshness;
    ucn_v6_binding_certificate_t joining_binding_certificate;
    uint64_t local_lease_deadline_us;
    ucn_v6_key_selector_t hop_current;
    ucn_v6_key_selector_t hop_previous;
    uint64_t hop_previous_deadline_us;
    ucn_v6_key_selector_t e2e_current;
    ucn_v6_key_selector_t e2e_previous;
    uint64_t e2e_previous_deadline_us;
    ucn_v6_replay_window_t hop_replay_current;
    ucn_v6_replay_window_t hop_replay_previous;
    ucn_v6_replay_window_t e2e_replay_current;
    ucn_v6_replay_window_t e2e_replay_previous;
    uint32_t hop_tx_next_sequence;
    uint32_t hop_tx_reserved_through;
    uint32_t e2e_tx_next_sequence;
    uint32_t e2e_tx_reserved_through;
} ucn_v6_security_session_record_t;

typedef struct ucn_v6_acl_key {
    ucn_v6_principal_t device_principal;
    ucn_v6_binding_key_t source_binding;
    ucn_v6_binding_key_t destination_binding;
    uint32_t session_generation;
    uint16_t source_endpoint;
    uint16_t destination_endpoint;
    ucn_v6_frame_type_t frame_type;
    uint16_t protocol_opcode;
    ucn_v6_traffic_class_t traffic_class;
    ucn_v6_delivery_guarantee_t delivery_guarantee;
    ucn_v6_interaction_role_t interaction_role;
    ucn_v6_operation_id_policy_t operation_id_policy;
    uint64_t exact_operation_id;
    ucn_v6_security_direction_t direction;
} ucn_v6_acl_key_t;

typedef struct ucn_v6_acl_entry {
    bool occupied;
    bool revoked;
    ucn_v6_acl_key_t key;
} ucn_v6_acl_entry_t;

typedef struct ucn_v6_group_policy_slot {
    ucn_v6_group_slot_state_t state;
    uint32_t group_id;
    uint32_t group_generation;
    ucn_v6_principal_t owner_principal;
} ucn_v6_group_policy_slot_t;

typedef struct ucn_v6_group_key_slot {
    ucn_v6_group_key_state_t state;
    bool requires_rekey;
    uint32_t group_id;
    uint32_t group_generation;
    uint16_t key_id;
    uint8_t suite_id;
    uint32_t current_generation;
    uint32_t previous_generation;
    uint64_t previous_deadline_us;
    ucn_v6_replay_window_t current_replay;
    ucn_v6_replay_window_t previous_replay;
    uint32_t tx_next_sequence;
    uint32_t tx_reserved_through;
} ucn_v6_group_key_slot_t;

typedef struct ucn_v6_group_replay_source {
    bool occupied;
    uint32_t group_id;
    uint32_t group_generation;
    uint16_t key_id;
    uint32_t key_generation;
    ucn_v6_binding_key_t claimed_source;
    uint32_t claimed_session_generation;
    ucn_v6_replay_window_t replay;
} ucn_v6_group_replay_source_t;

typedef struct ucn_v6_security_snapshot {
    uint32_t magic;
    uint16_t schema;
    uint16_t session_count;
    uint64_t snapshot_generation;
    uint32_t realm_id;
    ucn_v6_principal_t local_principal;
    bool local_binding_valid;
    ucn_v6_binding_key_t local_binding;
    bool authority_floor_valid;
    ucn_v6_authority_epoch_t authority_floor;
    ucn_v6_security_session_record_t
        sessions[UCN_V6_CONFIG_SECURITY_SESSIONS];
    ucn_v6_acl_entry_t acl_entries[UCN_V6_CONFIG_ACL_ENTRIES];
    ucn_v6_group_policy_slot_t
        groups[UCN_V6_CONFIG_STATIC_GROUP_SLOTS];
    ucn_v6_group_key_slot_t
        group_keys[UCN_V6_CONFIG_STATIC_GROUP_SLOTS]
                  [UCN_V6_CONFIG_GROUP_KEY_SLOTS];
    ucn_v6_group_replay_source_t
        group_replay_sources[UCN_V6_CONFIG_GROUP_REPLAY_SOURCES];
} ucn_v6_security_snapshot_t;

typedef struct ucn_v6_security_store_ops {
    void *context;
    ucn_v6_result_t (*load_witness)(
        void *context,
        ucn_v6_durable_generation_witness_t *witness);
    ucn_v6_result_t (*reserve_witness)(
        void *context,
        const ucn_v6_durable_generation_witness_t *witness);
    ucn_v6_result_t (*load)(
        void *context,
        ucn_v6_security_snapshot_t *snapshot);
    ucn_v6_result_t (*submit)(
        void *context,
        const ucn_v6_security_snapshot_t *snapshot);
} ucn_v6_security_store_ops_t;

typedef struct ucn_v6_security_crypto_ops {
    void *context;
    ucn_v6_result_t (*verify_proof)(
        void *context,
        ucn_v6_security_proof_role_t role,
        const ucn_v6_principal_t *principal,
        const uint8_t *canonical,
        size_t canonical_length,
        const uint8_t *proof,
        size_t proof_length);
    ucn_v6_result_t (*verify_tag)(
        void *context,
        const ucn_v6_key_selector_t *selector,
        const uint8_t *authenticated_data,
        size_t authenticated_data_length,
        const uint8_t *payload,
        size_t payload_length,
        const uint8_t tag[UCN_V6_SECURITY_TAG_BYTES]);
    ucn_v6_result_t (*compute_tag)(
        void *context,
        const ucn_v6_key_selector_t *selector,
        const uint8_t *authenticated_data,
        size_t authenticated_data_length,
        const uint8_t *payload,
        size_t payload_length,
        uint8_t tag[UCN_V6_SECURITY_TAG_BYTES]);
    ucn_v6_result_t (*seal_aead)(
        void *context,
        const ucn_v6_key_selector_t *selector,
        const uint8_t *authenticated_data,
        size_t authenticated_data_length,
        const uint8_t *plaintext,
        size_t plaintext_length,
        uint8_t *ciphertext,
        uint8_t tag[UCN_V6_SECURITY_TAG_BYTES]);
    ucn_v6_result_t (*open_aead)(
        void *context,
        const ucn_v6_key_selector_t *selector,
        const uint8_t *authenticated_data,
        size_t authenticated_data_length,
        const uint8_t *ciphertext,
        size_t ciphertext_length,
        const uint8_t tag[UCN_V6_SECURITY_TAG_BYTES],
        uint8_t *plaintext);
} ucn_v6_security_crypto_ops_t;

typedef struct ucn_v6_join_commit {
    ucn_v6_bootstrap_transcript_t transcript;
    ucn_v6_authority_epoch_t authority_epoch;
    ucn_v6_authority_freshness_t authority_freshness;
    ucn_v6_binding_certificate_t joining_binding_certificate;
    ucn_v6_binding_key_t local_binding;
    ucn_v6_binding_key_t peer_binding;
    uint32_t session_generation;
    uint16_t link_instance_id;
    uint32_t link_instance_generation;
    ucn_v6_key_selector_t hop_selector;
    ucn_v6_key_selector_t e2e_selector;
    ucn_v6_lease_verifier_policy_t authority_lease_policy;
    const uint8_t *device_proof;
    size_t device_proof_length;
    const uint8_t *authority_proof;
    size_t authority_proof_length;
    const uint8_t *peer_durable_receipt_proof;
    size_t peer_durable_receipt_proof_length;
    uint64_t peer_durable_receipt_generation;
} ucn_v6_join_commit_t;

typedef struct ucn_v6_security_manager ucn_v6_security_manager_t;
#ifndef UCN_V6_SECURITY_MANAGER_STORAGE_BYTES
#define UCN_V6_SECURITY_MANAGER_STORAGE_BYTES                             \
    ((size_t)(2048U + UCN_V6_CONFIG_SECURITY_SESSIONS * 768U +           \
              UCN_V6_CONFIG_ACL_ENTRIES * 128U +                         \
              UCN_V6_CONFIG_STATIC_GROUP_SLOTS *                         \
                  UCN_V6_CONFIG_GROUP_KEY_SLOTS * 192U +                  \
              UCN_V6_CONFIG_GROUP_REPLAY_SOURCES * 128U +                 \
              UCN_V6_SECURITY_INVALIDATION_DEPTH * 64U))
#endif
typedef union ucn_v6_security_manager_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_SECURITY_MANAGER_STORAGE_BYTES];
} ucn_v6_security_manager_storage_t;

typedef struct ucn_v6_security_view {
    uint64_t snapshot_generation;
    uint16_t admitted_sessions;
    uint16_t acl_entries;
    uint16_t active_groups;
    uint16_t active_group_keys;
    uint16_t pending_invalidations;
    ucn_v6_authority_epoch_t authority_floor;
    bool authority_floor_valid;
    bool faulted;
} ucn_v6_security_view_t;

typedef struct ucn_v6_security_open_result {
    /* EN: frame.payload is borrowed. For authenticated/plain frames it points
     * into encoded_frame; for decrypted frames it points into the caller's
     * plaintext_storage. It remains valid only while that backing storage is
     * unchanged. The result is a verified semantic DTO returned by Security,
     * not an unforgeable local capability and not a serializable cryptographic
     * proof object.  Security and every direct in-process consumer are one
     * trusted-computing-base boundary; arbitrary local memory corruption is
     * outside this API's attacker model.
     * 中文：frame.payload 为借用指针。认证明文帧指向 encoded_frame，解密帧
     * 指向调用方的 plaintext_storage；仅在对应后备存储未改变期间有效。
     * 此结果是 Security 返回的已验证语义 DTO，不是不可伪造的本地能力，也
     * 不是可序列化的密码学证明对象。Security 与所有进程内直接使用方属于
     * 同一可信计算基边界；任意本地内存破坏不属于此 API 的攻击者模型。 */
    ucn_v6_frame_t frame;
    ucn_v6_principal_t authenticated_principal;
    ucn_v6_session_key_t ingress_peer_session;
    /* EN: Immutable physical parent of ingress_peer_session.  Security fills
     * this from the verified Link Session; downstream fixed-capacity owners
     * use it to retire all children when that exact Link generation closes.
     * 中文：ingress_peer_session 的不可变物理父代际。Security 从已验证的
     * Link Session 填充；下游固定容量 Owner 用它在精确 Link 代际关闭时
     * 回收全部子资源。 */
    uint16_t ingress_link_instance_id;
    uint32_t ingress_link_instance_generation;
    bool hop_authenticated;
    bool endpoint_authorized;
    bool group_discovery_only;
} ucn_v6_security_open_result_t;

/* EN: Initializes a fixed, persistent Security Owner from caller storage.
 * 中文：从调用方静态存储初始化固定容量、可持久化的 Security Owner。 */
ucn_v6_result_t ucn_v6_security_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    uint32_t realm_id,
    const ucn_v6_principal_t *local_principal,
    const ucn_v6_security_store_ops_t *store,
    const ucn_v6_security_crypto_ops_t *crypto,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_security_manager_t **manager);

/* EN: Canonically serializes the complete mutually-authenticated transcript.
 * 中文：规范序列化完整的双向认证 transcript。 */
ucn_v6_result_t ucn_v6_security_write_bootstrap_transcript(
    const ucn_v6_bootstrap_transcript_t *transcript,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

/* EN: Exports the receipt domain only from an exact admitted session in the
 * manager's durably reloaded snapshot.  The returned generation is part of
 * the canonical bytes and must accompany the peer's receipt proof.
 * 中文：仅从 Manager 持久回读快照中的精确 ADMITTED Session 导出回执域。
 * 返回代际已经进入规范字节，必须随 Peer 回执证明一起传输。 */
ucn_v6_result_t ucn_v6_security_export_join_durable_receipt(
    const ucn_v6_security_manager_t *manager,
    const ucn_v6_principal_t *peer_principal,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length,
    uint64_t *durable_record_generation);

/* EN: Verifies both principals and durably installs the sole ADMITTED session.
 * 中文：验证双方 Principal，并持久安装唯一可写 ADMITTED Session。 */
ucn_v6_result_t ucn_v6_security_commit_join(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_bootstrap_owner_t *bootstrap_owner,
    const ucn_v6_bootstrap_key_t *bootstrap_key,
    uint64_t now_us,
    const ucn_v6_join_commit_t *commit);

/* EN: Fences an admitted Peer after Link loss/change until authenticated
 * REAUTH installs the exact-next Session and Link generations.
 * 中文：Link 丢失或变化后撤销 Peer 准入，直到认证 REAUTH 安装精确后继
 * Session/Link 代际。 */
ucn_v6_result_t ucn_v6_security_require_reauth(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_principal_t *peer_principal);

/* EN: Applies one canonical Link-generation invalidation.  Every durable
 * Session whose exact physical parent is {link_id, link_generation} is
 * fenced for REAUTH in one fixed-capacity snapshot commit; only after that
 * commit succeeds are the corresponding child SESSION invalidations made
 * visible through invalidation_peek().  A stale Link generation is an
 * idempotent no-op.  The current Store contract is synchronous (there is no
 * poll callback); Provider failure faults the Manager without publishing a
 * partially completed child cascade.
 * 中文：应用一个规范 Link 代际失效事件。所有物理父域精确等于
 * {link_id, link_generation} 的持久 Session，会在一次固定容量快照提交中
 * 统一进入 REAUTH Fence；仅持久提交成功后，才通过 invalidation_peek()
 * 发布对应的子 SESSION 失效事件。过期 Link 代际是幂等空操作。当前 Store
 * 合同为同步调用（没有 poll 回调）；Provider 失败会使 Manager 进入故障
 * Fence，但不会发布部分完成的子级联。 */
ucn_v6_result_t ucn_v6_security_apply_link_invalidation(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_stack_invalidation_t *link_invalidation);

/* EN: Installs or revokes an exact ACL entry under authenticated admin proof.
 * 中文：在认证管理证明下安装或撤销精确 ACL 项。 */
ucn_v6_result_t ucn_v6_security_set_acl(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_acl_entry_t *entry,
    const ucn_v6_principal_t *admin_principal,
    const uint8_t *admin_proof,
    size_t admin_proof_length);

/* EN: Installs, advances, or permanently retires one fixed Group policy slot.
 * 中文：安装、推进或永久退休一个固定 Group 策略槽。 */
ucn_v6_result_t ucn_v6_security_set_group_policy(
    ucn_v6_security_manager_t *manager,
    size_t group_slot,
    const ucn_v6_group_policy_slot_t *policy,
    const ucn_v6_principal_t *admin_principal,
    const uint8_t *admin_proof,
    size_t admin_proof_length);

/* EN: Rotates or retires a fixed Group key slot with persist-before-use.
 * 中文：按先持久化后使用原则轮换或退休固定 Group Key 槽。 */
ucn_v6_result_t ucn_v6_security_set_group_key(
    ucn_v6_security_manager_t *manager,
    uint64_t now_us,
    size_t group_slot,
    size_t key_slot,
    const ucn_v6_group_key_slot_t *key,
    const ucn_v6_principal_t *admin_principal,
    const uint8_t *admin_proof,
    size_t admin_proof_length);

/* EN: Rotates the Peer/E2E selectors of one admitted session atomically.
 * 中文：原子轮换一个已准入 Session 的 Peer/E2E selector。 */
ucn_v6_result_t ucn_v6_security_rotate_session_keys(
    ucn_v6_security_manager_t *manager,
    uint64_t now_us,
    const ucn_v6_principal_t *peer_principal,
    const ucn_v6_key_selector_t *next_hop,
    const ucn_v6_key_selector_t *next_e2e,
    uint64_t previous_grace_us,
    const ucn_v6_principal_t *admin_principal,
    const uint8_t *admin_proof,
    size_t admin_proof_length);

/* EN: Persistently revokes a session; a revoked slot is never reused.
 * 中文：持久撤销 Session；已撤销槽永久不复用。 */
ucn_v6_result_t ucn_v6_security_revoke_session(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_principal_t *peer_principal,
    const ucn_v6_principal_t *admin_principal,
    const uint8_t *admin_proof,
    size_t admin_proof_length);

/* EN: Peeks the oldest durable-session invalidation without removing it;
 * exact acknowledgement is the only operation that advances the queue.
 * 中文：非破坏性查看最早的持久 Session 失效事件；只有精确确认才推进队列。 */
ucn_v6_result_t ucn_v6_security_invalidation_peek(
    const ucn_v6_security_manager_t *manager,
    ucn_v6_stack_invalidation_t *invalidation);
ucn_v6_result_t ucn_v6_security_invalidation_ack(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_stack_invalidation_t *invalidation);

/* EN: Verifies hop/group auth, replay, optional E2E protection, and exact ACL.
 * Encoded input, plaintext storage, and result must be pairwise disjoint.
 * Plaintext storage is caller-owned and may be modified on a rejected AEAD
 * operation after this admission check; result is never written on rejection.
 * 中文：验证逐跳/组认证、重放、可选 E2E 与精确 ACL。编码输入、明文暂存区
 * 与 result 必须两两互不重叠；AEAD 被拒绝时明文暂存区可能改变，但 result
 * 保持不写回。 */
ucn_v6_result_t ucn_v6_security_open_frame(
    ucn_v6_security_manager_t *manager,
    uint64_t now_us,
    uint16_t ingress_link_instance_id,
    uint32_t ingress_link_instance_generation,
    const ucn_v6_principal_t *authenticated_peer_principal,
    const uint8_t *encoded_frame,
    size_t encoded_length,
    uint8_t *plaintext_storage,
    size_t plaintext_capacity,
    ucn_v6_security_open_result_t *result);

/* EN: Reserves a persistent sequence, seals E2E, then applies the next-hop
 * tag. Frame, input payload, every work/output buffer, and output-length
 * object must be pairwise disjoint. Work storage may change on failure;
 * output is copied only after every security step succeeds.
 * 中文：持久预留 Sequence，完成 E2E 封装后生成下一跳 Tag。Frame、输入
 * Payload、各工作/输出缓冲区与长度对象必须两两互不重叠；工作区在失败时
 * 可改变，最终输出仅在全部安全步骤成功后写入。 */
ucn_v6_result_t ucn_v6_security_protect_frame(
    ucn_v6_security_manager_t *manager,
    uint64_t now_us,
    const ucn_v6_principal_t *next_hop_principal,
    const ucn_v6_principal_t *e2e_peer_principal,
    ucn_v6_frame_t *frame,
    uint8_t *payload_work,
    size_t payload_work_capacity,
    uint8_t *frame_work,
    size_t frame_work_capacity,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

/* EN: Verifies the raw ingress frame and replay state, then replaces only
 * mutable hop state. The original source, origin sequence, ciphertext and
 * E2E tag remain byte-for-byte unchanged; a fresh next-hop sequence and Link
 * tag are used. verified_ingress returns the Security-produced in-process
 * admission result for immediate Route/QoS use; this API never accepts a
 * caller-supplied result as proof. Its frame payload borrows encoded_frame and
 * must not outlive or out-mutate that storage. Ingress bytes, frame_work,
 * output, output-length and both result objects must be pairwise disjoint;
 * rejected calls never write output/result objects.
 * 中文：先在 Security 内部验证原始入站帧与重放状态，再仅替换可变
 * 逐跳状态。原始 Source、Origin 序号、密文和 E2E Tag 保持不变，
 * 并使用新的下一跳序号与 Link Tag。verified_ingress 是供 Route/QoS 立即
 * 使用的进程内准入结果，本 API 不接受调用方预先传入的结果充当证明；其中
 * 的帧 Payload 借用 encoded_frame，不得超过或改写该存储的生命周期。
 * 原始入站字节、frame_work、output、长度对象与两个结果对象必须两两互不
 * 重叠；拒绝路径不会写回输出或结果对象。 */
ucn_v6_result_t ucn_v6_security_relay_frame(
    ucn_v6_security_manager_t *manager,
    uint64_t now_us,
    uint16_t ingress_link_instance_id,
    uint32_t ingress_link_instance_generation,
    const ucn_v6_principal_t *authenticated_peer_principal,
    const ucn_v6_principal_t *next_hop_principal,
    uint64_t hop_budget_debit_us,
    const uint8_t *encoded_frame,
    size_t encoded_length,
    uint8_t *frame_work,
    size_t frame_work_capacity,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length,
    ucn_v6_security_open_result_t *verified_ingress,
    ucn_v6_frame_t *relayed_frame);

/* EN: Protects one exact hop-authenticated HELLO/Capability control frame.
 * Frame, payload, work/output bytes, and output-length must be pairwise
 * disjoint. This path never grants Endpoint ACL or admission authority.
 * 中文：保护一帧精确定义的逐跳认证 HELLO/Capability 控制帧；Frame、
 * Payload、工作/输出字节与长度对象必须两两互不重叠；该路径绝不授予
 * Endpoint ACL 或准入权。 */
ucn_v6_result_t ucn_v6_security_protect_peer_discovery(
    ucn_v6_security_manager_t *manager,
    uint64_t now_us,
    const ucn_v6_principal_t *peer_principal,
    ucn_v6_frame_t *frame,
    uint8_t *frame_work,
    size_t frame_work_capacity,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

/* EN: Protects the unique non-forwardable Group HELLO contract. Frame,
 * payload, work/output bytes, and output-length must be pairwise disjoint.
 * 中文：保护唯一、禁止转发的 Group HELLO 合同；Frame、Payload、工作/
 * 输出字节与长度对象必须两两互不重叠。 */
ucn_v6_result_t ucn_v6_security_protect_group_hello(
    ucn_v6_security_manager_t *manager,
    size_t group_slot,
    size_t key_slot,
    ucn_v6_frame_t *frame,
    uint8_t *frame_work,
    size_t frame_work_capacity,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

/* EN: Copies read-only diagnostics from validated opaque state.
 * 中文：从已校验的 opaque 状态复制只读诊断。 */
ucn_v6_result_t ucn_v6_security_copy_view(
    const ucn_v6_security_manager_t *manager,
    ucn_v6_security_view_t *view);

#ifdef __cplusplus
}
#endif

#endif
