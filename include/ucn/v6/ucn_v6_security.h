#ifndef UCN_V6_SECURITY_H
#define UCN_V6_SECURITY_H

#include "ucn/v6/ucn_v6_bootstrap.h"
#include "ucn/v6/ucn_v6_config.h"
#include "ucn/v6/ucn_v6_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_SECURITY_PROOF_MAX_BYTES ((size_t)96U)
#define UCN_V6_SECURITY_REPLAY_BITS ((uint8_t)64U)
#define UCN_V6_SECURITY_SNAPSHOT_MAGIC UINT32_C(0x56533653)
#define UCN_V6_SECURITY_SNAPSHOT_SCHEMA UINT16_C(1)

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
    UCN_V6_PROOF_REALM_ADMIN = 3
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
    uint32_t durable_reserved_through;
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
    uint32_t link_instance_generation;
    ucn_v6_bootstrap_flow_t bootstrap_flow;
    uint64_t bootstrap_transaction_id;
    uint64_t bootstrap_device_nonce;
    uint64_t bootstrap_authority_nonce;
    uint64_t bootstrap_freshness_nonce;
    uint8_t bootstrap_prior_messages_hash[32];
    ucn_v6_e2e_mode_t e2e_mode;
    ucn_v6_authority_epoch_t authority_epoch;
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
    uint32_t tx_next_sequence;
    uint32_t tx_reserved_through;
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
    ucn_v6_result_t (*load_generation_witness)(
        void *context,
        uint64_t *generation);
    ucn_v6_result_t (*reserve_generation_witness)(
        void *context,
        uint64_t generation);
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
    ucn_v6_binding_certificate_t joining_binding_certificate;
    ucn_v6_binding_key_t local_binding;
    ucn_v6_binding_key_t peer_binding;
    uint32_t session_generation;
    uint32_t link_instance_generation;
    ucn_v6_key_selector_t hop_selector;
    ucn_v6_key_selector_t e2e_selector;
    uint64_t authority_local_lease_deadline_us;
    const uint8_t *device_proof;
    size_t device_proof_length;
    const uint8_t *authority_proof;
    size_t authority_proof_length;
} ucn_v6_join_commit_t;

typedef struct ucn_v6_security_manager ucn_v6_security_manager_t;
#ifndef UCN_V6_SECURITY_MANAGER_STORAGE_BYTES
#define UCN_V6_SECURITY_MANAGER_STORAGE_BYTES                             \
    ((size_t)(2048U + UCN_V6_CONFIG_SECURITY_SESSIONS * 512U +           \
              UCN_V6_CONFIG_ACL_ENTRIES * 128U +                         \
              UCN_V6_CONFIG_STATIC_GROUP_SLOTS *                         \
                  UCN_V6_CONFIG_GROUP_KEY_SLOTS * 192U +                  \
              UCN_V6_CONFIG_GROUP_REPLAY_SOURCES * 128U))
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
    bool faulted;
} ucn_v6_security_view_t;

typedef struct ucn_v6_security_open_result {
    ucn_v6_frame_t frame;
    ucn_v6_principal_t authenticated_principal;
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

/* EN: Verifies hop/group auth, replay, optional E2E protection, and exact ACL.
 * Plaintext storage is caller-owned and may be modified on a rejected AEAD
 * operation; result is never written on rejection.
 * 中文：验证逐跳/组认证、重放、可选 E2E 与精确 ACL。AEAD 被拒绝时调用方
 * 明文暂存区可能改变，但 result 保持不写回。 */
ucn_v6_result_t ucn_v6_security_open_frame(
    ucn_v6_security_manager_t *manager,
    uint64_t now_us,
    uint32_t ingress_link_instance_generation,
    const ucn_v6_principal_t *authenticated_peer_principal,
    const uint8_t *encoded_frame,
    size_t encoded_length,
    uint8_t *plaintext_storage,
    size_t plaintext_capacity,
    ucn_v6_security_open_result_t *result);

/* EN: Reserves a persistent sequence, seals E2E, then applies the next-hop
 * tag. Work storage must be at least the final frame size and may change on
 * failure; output is copied only after every security step succeeds.
 * 中文：持久预留 Sequence，完成 E2E 封装后生成下一跳 Tag。工作区在失败时
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

/* EN: Protects one exact hop-authenticated HELLO/Capability control frame.
 * This path never grants Endpoint ACL or admission authority.
 * 中文：保护一帧精确定义的逐跳认证 HELLO/Capability 控制帧；该路径绝不
 * 授予 Endpoint ACL 或准入权。 */
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

/* EN: Protects the unique non-forwardable Group HELLO contract.
 * 中文：保护唯一、禁止转发的 Group HELLO 合同。 */
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
