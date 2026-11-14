#ifndef UCN_INTERNAL_SECURITY_H
#define UCN_INTERNAL_SECURITY_H

#include "internal/ucn_owner.h"
#include "internal/ucn_control_wire.h"
#include "internal/ucn_digest.h"
#include "ucn/ucn_config.h"

#define UCN_I_SECURITY_SCHEMA UINT16_C(1)
#define UCN_I_SECURITY_RECORD_SCHEMA UINT16_C(1)
#define UCN_I_SECURITY_RECORD_SCHEMA_ID UINT16_C(0x5301)
#define UCN_I_SECURITY_OPERATION_KIND UINT16_C(0x0501)
#define UCN_I_SECURITY_RECORD_BYTES 192U
#define UCN_I_SECURITY_PROOF_BYTES 128U
#define UCN_I_SECURITY_PRINCIPAL_BYTES 16U
#define UCN_I_SECURITY_FINGERPRINT_BYTES 16U
#define UCN_I_SECURITY_TRANSCRIPT_BYTES 32U
#define UCN_I_SECURITY_SESSION_ACL_MAX 64U
#define UCN_I_PEER_REAUTH_RECORD_BYTES 208U
/* EN: Canonical structural bytes only, never an authenticated peer or Session.
 * 中文：仅规范结构字节，不是已认证邻居或Session。 */
typedef struct ucn_i_peer_reauth_record {
    uint8_t canonical[UCN_I_PEER_REAUTH_RECORD_BYTES];
} ucn_i_peer_reauth_record_t;
/* EN: Strict canonical decode/copy; errors preserve output, aliases reject.
 * 中文：严格规范解码/复制；失败不写输出，拒绝任何别名。 */
ucn_result_t ucn_i_peer_reauth_decode(const uint8_t *input, size_t length,
                                    ucn_i_peer_reauth_record_t *output);
/* EN: Encode only validated canonical bytes, no authentication grant.
 * 中文：仅编码校验后的规范字节，不授予认证。 */
ucn_result_t ucn_i_peer_reauth_encode(const ucn_i_peer_reauth_record_t *input,
                                    uint8_t *output, size_t capacity);
#define UCN_I_PEER_REAUTH_FRAGMENT_PREFIX 18U
typedef struct ucn_i_peer_reauth_fragment {
    uint64_t transaction;
    uint16_t total_bytes;
    uint8_t stage;
    uint8_t index;
    uint8_t data_bytes;
    uint8_t data[80];
} ucn_i_peer_reauth_fragment_t;
/* EN: Structural fragment only; full logical signature remains mandatory.
 * 中文：仅结构分片；完整逻辑签名仍是必要门禁。 */
ucn_result_t ucn_i_peer_reauth_fragment_encode(const ucn_i_peer_reauth_fragment_t *input,
    uint8_t *output,size_t capacity,size_t *written);
/* EN: Strict canonical decode, zero writes on error.
 * 中文：严格规范解码，失败零写入。 */
ucn_result_t ucn_i_peer_reauth_fragment_decode(const uint8_t *input,size_t length,
    ucn_i_peer_reauth_fragment_t *output);
#define UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES 72U
#define UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES 44U
#define UCN_I_SECURITY_ORIGIN_TAG_BYTES 16U
#define UCN_I_SECURITY_HOP_TAG_BYTES 12U
#define UCN_I_SECURITY_HOP_TRAILER_BYTES 16U
#define UCN_I_SECURITY_HOP_AAD_BYTES (UCN_ADAPTER_FRAME_BYTES + 35U)

#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_I_SECURITY_SESSION_COUNT 2U
#define UCN_I_SECURITY_REPLAY_RESERVATION_COUNT 2U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_I_SECURITY_SESSION_COUNT 8U
#define UCN_I_SECURITY_REPLAY_RESERVATION_COUNT 4U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define UCN_I_SECURITY_SESSION_COUNT 16U
#define UCN_I_SECURITY_REPLAY_RESERVATION_COUNT 8U
#else
#error "UCN_PROFILE must be UCN_PROFILE_NANO, UCN_PROFILE_LITE, or UCN_PROFILE_FULL"
#endif

typedef uint8_t ucn_i_security_level_t;
enum {
    UCN_I_SECURITY_AUTHENTICATED = 1,
    UCN_I_SECURITY_CONFIDENTIAL = 2
};

typedef uint8_t ucn_i_hop_profile_t;
enum {
    UCN_I_HOP_PROFILE_H0 = 0,
    UCN_I_HOP_PROFILE_H1 = 1,
    UCN_I_HOP_PROFILE_H2 = 2,
    UCN_I_HOP_PROFILE_H3 = 3
};

typedef uint8_t ucn_i_security_session_phase_t;
enum {
    UCN_I_SECURITY_SESSION_EMPTY = 0,
    UCN_I_SECURITY_SESSION_AWAITING_DURABILITY = 1,
    UCN_I_SECURITY_SESSION_ACTIVE = 2,
    UCN_I_SECURITY_SESSION_FENCED = 3
};

typedef struct ucn_i_security_binding {
    uint32_t address;
    uint32_t binding_generation;
    uint8_t principal[UCN_I_SECURITY_PRINCIPAL_BYTES];
} ucn_i_security_binding_t;

typedef struct ucn_i_security_key_selector {
    uint32_t key_generation;
    uint16_t key_id;
    uint8_t suite_id;
    uint8_t reserved_zero;
} ucn_i_security_key_selector_t;

typedef struct ucn_i_security_candidate {
    ucn_i_security_binding_t local;
    ucn_i_security_binding_t peer;
    uint64_t expires_at_us;
    uint32_t link_generation;
    uint32_t session_generation;
    uint32_t policy_generation;
    ucn_i_security_key_selector_t origin_tx;
    ucn_i_security_key_selector_t origin_rx;
    ucn_i_security_key_selector_t hop_tx;
    ucn_i_security_key_selector_t hop_rx;
    uint8_t origin_fingerprint[UCN_I_SECURITY_FINGERPRINT_BYTES];
    uint8_t hop_fingerprint[UCN_I_SECURITY_FINGERPRINT_BYTES];
    uint8_t transcript_digest[UCN_I_SECURITY_TRANSCRIPT_BYTES];
    uint8_t origin_level;
    uint8_t hop_profile;
    uint8_t address_width;
    uint8_t reserved_zero;
} ucn_i_security_candidate_t;

typedef struct ucn_i_security_domain_rule {
    uint64_t domain_id;
    uint8_t peer_principal[UCN_I_SECURITY_PRINCIPAL_BYTES];
} ucn_i_security_domain_rule_t;

typedef struct ucn_i_security_durability_base {
    uint64_t domain_id;
    uint64_t next_transaction_id;
    uint64_t expected_record_generation;
    uint64_t absolute_deadline_us;
    uint64_t prior_session_generation;
    uint16_t domain_generation;
    uint16_t reserved_zero;
    uint32_t volatile_continuation;
    uint8_t expected_body_digest[16];
} ucn_i_security_durability_base_t;

typedef struct ucn_i_security_requirement_view {
    const uint8_t *canonical_body;
    uint64_t domain_id;
    uint64_t foundation_transaction_id;
    uint64_t expected_record_generation;
    uint64_t absolute_deadline_us;
    uint64_t transition_fingerprint;
    uint32_t runtime_instance;
    uint32_t body_bytes;
    uint32_t volatile_continuation;
    uint16_t caller_owner_instance;
    uint16_t domain_generation;
    uint16_t schema_id;
    uint16_t schema_version;
    uint16_t operation_kind;
    uint8_t expected_body_digest[16];
} ucn_i_security_requirement_view_t;

typedef struct ucn_i_security_handle {
    uint32_t runtime_instance;
    uint16_t owner_instance;
    uint16_t slot;
    uint32_t slot_generation;
    uint32_t session_generation;
} ucn_i_security_handle_t;

typedef struct ucn_i_security_session_view {
    ucn_i_security_binding_t peer;
    uint64_t expires_at_us;
    uint32_t link_generation;
    uint32_t session_generation;
    uint32_t policy_generation;
    uint32_t origin_tx_key_generation;
    uint32_t origin_rx_key_generation;
    uint8_t origin_level;
    uint8_t hop_profile;
    uint8_t phase;
    uint8_t reserved_zero;
} ucn_i_security_session_view_t;

typedef uint8_t ucn_i_security_access_direction_t;
enum {
    UCN_I_SECURITY_ACCESS_INBOUND = 1,
    UCN_I_SECURITY_ACCESS_OUTBOUND = 2
};

typedef struct ucn_i_security_acl_rule {
    uint8_t peer_principal[UCN_I_SECURITY_PRINCIPAL_BYTES];
    uint8_t context_fingerprint[UCN_I_SECURITY_FINGERPRINT_BYTES];
    uint32_t peer_binding_generation;
    uint16_t service_id;
    uint16_t protocol_opcode;
    uint8_t direction;
    uint8_t reserved_zero[3];
} ucn_i_security_acl_rule_t;

typedef struct ucn_i_security_access_request {
    ucn_i_security_handle_t session;
    uint8_t context_fingerprint[UCN_I_SECURITY_FINGERPRINT_BYTES];
    uint16_t service_id;
    uint16_t protocol_opcode;
    uint8_t direction;
    uint8_t reserved_zero[3];
} ucn_i_security_access_request_t;

/* EN: C0 has no Service ID.  Its ACL key is the exact authenticated Peer,
 * Binding, context fingerprint, opcode and direction; ACL rules represent
 * this domain with service_id == 0 and a nonzero registered opcode.
 * 中文：C0 没有 Service ID。其 ACL 键由精确认证 Peer、Binding、上下文摘要、
 * Opcode 和方向组成；ACL 规则使用 service_id == 0 且非零已登记 Opcode 表示
 * 该域。 */
typedef struct ucn_i_security_control_access_request {
    ucn_i_security_handle_t session;
    uint8_t context_fingerprint[UCN_I_SECURITY_FINGERPRINT_BYTES];
    uint16_t protocol_opcode;
    uint8_t direction;
    uint8_t reserved_zero;
} ucn_i_security_control_access_request_t;

typedef struct ucn_i_security_current_facts {
    ucn_i_security_binding_t local;
    ucn_i_security_binding_t peer;
    uint64_t now_us;
    uint32_t link_generation;
    uint32_t policy_generation;
} ucn_i_security_current_facts_t;

/* EN: A Control FSM owns and persists the C0 Transaction ID.  Security owns
 * only canonical binding, protection and the exact ACL check; it must never
 * allocate a second transaction counter for the same Wire field.
 * 中文：C0 Transaction ID 由对应 Control FSM 唯一分配并按需持久化。Security
 * 只负责 canonical 绑定、密码保护与精确 ACL；不得为同一线上字段再维护第二套
 * 事务计数器。 */
typedef struct ucn_i_security_c0_protect_request {
    ucn_i_security_handle_t session;
    ucn_i_security_current_facts_t facts;
    uint64_t transaction_id;
    uint16_t protocol_opcode;
    uint8_t context_fingerprint[UCN_I_SECURITY_FINGERPRINT_BYTES];
    uint8_t traffic_class;
    uint8_t delivery_guarantee;
    uint8_t interaction_role;
    uint8_t payload_kind;
    uint8_t hop_limit;
    uint8_t direction;
    uint8_t reserved_zero[2];
} ucn_i_security_c0_protect_request_t;

/* EN: Canonical C1 material is assembled from authenticated Session facts,
 * never from a bare address-to-principal guess.
 * 中文：C1 canonical 材料只能由已认证 Session 事实构造，不能根据裸地址猜测
 * Principal。 */
typedef struct ucn_i_security_c1_origin_material {
    ucn_i_security_handle_t session;
    ucn_i_security_current_facts_t facts;
    size_t payload_bytes;
    uint32_t source_address;
    uint32_t destination_address;
    uint32_t origin_sequence;
    uint16_t service_id;
    uint16_t protocol_opcode;
    uint8_t common_header[3];
    uint8_t direction;
    uint8_t reserved_zero[3];
} ucn_i_security_c1_origin_material_t;

/* EN: This is a Coordinator-routed, immutable projection of a Persistence
 * reload proof. Security never receives the Persistence Owner pointer and
 * cannot submit storage I/O directly.
 * 中文：这是经 Coordinator 路由的 Persistence reload proof 不可变投影。
 * Security 不持有 Persistence Owner 指针，也不能直接提交存储 I/O。 */
typedef struct ucn_i_security_durability_proof {
    ucn_handle_t persistence_handle;
    uint64_t domain_id;
    uint64_t record_generation;
    uint64_t foundation_transaction_id;
    uint64_t witness_generation;
    uint64_t transition_fingerprint;
    uint32_t runtime_instance;
    uint32_t body_bytes;
    uint32_t volatile_continuation;
    uint16_t persistence_owner_instance;
    uint16_t caller_owner_instance;
    uint16_t domain_generation;
    uint16_t schema_id;
    uint16_t schema_version;
    uint16_t operation_kind;
    uint16_t reserved_zero;
    uint8_t body_digest[16];
} ucn_i_security_durability_proof_t;

typedef struct ucn_i_security_replay_handle {
    ucn_i_security_handle_t session;
    uint64_t sequence;
    uint32_t reservation_generation;
    uint32_t hop_sequence;
    uint32_t hop_reservation_generation;
    uint16_t reservation_slot;
    uint16_t hop_reservation_slot;
    uint8_t has_hop;
    uint8_t replay_domain; /* 0=C1+ Origin, 1=C0; independent replay namespaces. */
    uint8_t reserved_zero[2];
} ucn_i_security_replay_handle_t;

/* EN: A relay owns only the inbound hop replay reservation.  It must not
 * fabricate an origin replay reservation because relays never authenticate
 * or decrypt the end-to-end payload.
 * 中文：中继只持有入站逐跳 Replay 预留。中继不会认证或解密端到端 Payload，
 * 因而绝不能伪造 Origin Replay 预留。 */
typedef struct ucn_i_security_hop_replay_handle {
    ucn_i_security_handle_t session;
    uint32_t hop_sequence;
    uint32_t reservation_generation;
    uint16_t reservation_slot;
    uint16_t reserved_zero;
} ucn_i_security_hop_replay_handle_t;

typedef struct ucn_i_security_tx_handle {
    ucn_i_security_handle_t session;
    uint64_t deadline_us;
    uint32_t origin_sequence;
    uint32_t reservation_generation;
} ucn_i_security_tx_handle_t;

typedef struct ucn_i_security_tx_reservation {
    uint64_t deadline_us;
    uint8_t context_fingerprint[UCN_I_SECURITY_FINGERPRINT_BYTES];
    uint32_t origin_sequence;
    uint32_t generation;
    uint16_t service_id;
    uint16_t protocol_opcode;
    uint8_t direction;
    uint8_t valid;
    uint8_t reserved_zero[2];
} ucn_i_security_tx_reservation_t;

typedef struct ucn_i_security_replay_reservation {
    uint64_t sequence;
    uint64_t deadline_us;
    uint8_t aad_digest[16];
    uint8_t payload_digest[16];
    uint32_t generation;
    uint8_t valid;
    /* EN: Only a typed Composition handler may publish business completion.
     * The generic authenticated-control boundary must not turn a structurally
     * valid but unsupported body into a committed replay entry.
     * 中文：只有强类型 Composition Handler 才能发布业务完成。通用认证控制
     * 边界不得把“结构合法但语义未实现”的 Body 直接提交为已消费 Replay。 */
    uint8_t business_completed;
    uint8_t replay_domain;
    uint8_t reserved_zero;
} ucn_i_security_replay_reservation_t;

typedef struct ucn_i_security_replay_window {
    uint64_t highest_committed;
    uint64_t committed_bitmap;
    ucn_i_security_replay_reservation_t
        reservations[UCN_I_SECURITY_REPLAY_RESERVATION_COUNT];
} ucn_i_security_replay_window_t;

typedef ucn_result_t (*ucn_i_security_verify_session_fn)(
    void *context,
    const ucn_i_security_candidate_t *candidate,
    const uint8_t *proof,
    size_t proof_bytes);

typedef struct ucn_i_security_origin_protect_request {
    ucn_i_security_key_selector_t selector;
    const uint8_t *nonce_input;
    const uint8_t *aad;
    const uint8_t *plaintext;
    size_t nonce_input_bytes;
    size_t aad_bytes;
    size_t payload_bytes;
    uint8_t *protected_payload;
    uint8_t *origin_tag;
} ucn_i_security_origin_protect_request_t;

typedef struct ucn_i_security_origin_open_request {
    ucn_i_security_key_selector_t selector;
    const uint8_t *nonce_input;
    const uint8_t *aad;
    const uint8_t *protected_payload;
    const uint8_t *origin_tag;
    size_t nonce_input_bytes;
    size_t aad_bytes;
    size_t payload_bytes;
    uint8_t *plaintext;
} ucn_i_security_origin_open_request_t;

typedef ucn_result_t (*ucn_i_security_protect_origin_fn)(
    void *context,
    const ucn_i_security_origin_protect_request_t *request);
typedef ucn_result_t (*ucn_i_security_open_origin_fn)(
    void *context,
    const ucn_i_security_origin_open_request_t *request);

typedef struct ucn_i_security_hop_request {
    ucn_i_security_key_selector_t selector;
    const uint8_t *aad;
    size_t aad_bytes;
    uint8_t *hop_tag;
} ucn_i_security_hop_request_t;

typedef struct ucn_i_security_hop_verify_request {
    ucn_i_security_key_selector_t selector;
    const uint8_t *aad;
    const uint8_t *hop_tag;
    size_t aad_bytes;
} ucn_i_security_hop_verify_request_t;

typedef ucn_result_t (*ucn_i_security_protect_hop_fn)(
    void *context,
    const ucn_i_security_hop_request_t *request);
typedef ucn_result_t (*ucn_i_security_verify_hop_fn)(
    void *context,
    const ucn_i_security_hop_verify_request_t *request);

typedef struct ucn_i_security_provider {
    uint16_t struct_size;
    uint16_t api_version;
    void *context;
    ucn_i_security_verify_session_fn verify_session;
    ucn_i_security_protect_origin_fn protect_origin;
    ucn_i_security_open_origin_fn open_origin;
    ucn_i_security_protect_hop_fn protect_hop;
    ucn_i_security_verify_hop_fn verify_hop;
} ucn_i_security_provider_t;

typedef struct ucn_i_security_config {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint16_t owner_instance;
    uint16_t persistence_business_owner_instance;
    uint8_t address_width;
    uint8_t reserved_zero[3];
    const ucn_i_security_domain_rule_t *domain_rules;
    uint16_t domain_rule_count;
    uint16_t reserved_zero2;
    const ucn_i_security_acl_rule_t *acl_rules;
    uint16_t acl_rule_count;
    uint16_t reserved_zero3;
    uint64_t replay_reservation_lifetime_us;
    ucn_i_security_provider_t provider;
    ucn_i_lock_ops_t state_lock;
    ucn_i_callback_gate_t *provider_gate;
} ucn_i_security_config_t;

typedef struct ucn_i_security_slot {
    const ucn_i_security_acl_rule_t *session_acl;
    uint16_t session_acl_count;
    ucn_i_security_candidate_t candidate;
    ucn_i_security_durability_base_t durability;
    uint8_t body[UCN_I_SECURITY_RECORD_BYTES];
    uint64_t transition_fingerprint;
    ucn_handle_t persistence_handle;
    uint8_t expected_published_digest[16];
    ucn_i_security_replay_window_t origin_replay;
    /* EN: Separate C0 committed window, shared bounded reservation pool.
     * 中文：C0独立已提交窗口，共用有界预留槽，不增加报文头。 */
    uint64_t c0_rx_highest_committed;
    uint64_t c0_rx_committed_bitmap;
    ucn_i_security_replay_window_t hop_replay;
    ucn_i_security_tx_reservation_t origin_tx_reservation;
    /* EN: C0 transaction identifiers are monotonically consumed inside one
     * active Session/direction.  A Driver retry must reuse the already sealed
     * frame and must never call the crypto Provider again with the same nonce.
     * 中文：同一活动 Session/方向内的 C0 Transaction ID 单调消耗。Driver
     * 重试必须复用已封装帧，绝不能用同一 nonce 再次调用密码 Provider。 */
    uint64_t c0_tx_highest_consumed;
    uint32_t origin_tx_highest_reserved;
    uint32_t hop_tx_highest_reserved;
    uint32_t slot_generation;
    uint8_t occupied;
    uint8_t phase;
    uint8_t persistence_bound;
    uint8_t reserved_zero;
} ucn_i_security_slot_t;

typedef struct ucn_i_security_owner {
    uint32_t magic;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint32_t next_operation_id;
    uint16_t schema;
    uint16_t owner_instance;
    uint16_t persistence_business_owner_instance;
    uint8_t address_width;
    uint8_t callback_active;
    uint16_t callback_slot;
    const ucn_i_security_domain_rule_t *domain_rules;
    uint16_t domain_rule_count;
    uint16_t callback_proof_bytes;
    const ucn_i_security_acl_rule_t *acl_rules;
    uint16_t acl_rule_count;
    uint16_t replay_maintenance_cursor;
    uint64_t replay_reservation_lifetime_us;
    ucn_i_security_provider_t provider;
    ucn_i_lock_ops_t state_lock;
    ucn_i_callback_gate_t *provider_gate;
    ucn_i_callback_claim_t callback_claim;
    ucn_i_security_candidate_t callback_candidate;
    uint8_t callback_proof[UCN_I_SECURITY_PROOF_BYTES];
    ucn_i_security_slot_t sessions[UCN_I_SECURITY_SESSION_COUNT];
} ucn_i_security_owner_t;

typedef struct ucn_i_security_codec_workspace {
    ucn_i_security_candidate_t candidate;
} ucn_i_security_codec_workspace_t;

#define UCN_I_SECURITY_REAUTH_MESSAGE_BYTES 227U
typedef ucn_result_t (*ucn_i_security_reauth_verify_fn)(void *, const uint8_t[16],
    const uint8_t[UCN_I_SECURITY_REAUTH_MESSAGE_BYTES], const uint8_t *, uint8_t);
/* Exact Foundation body and its decoded candidate are borrowed together.
 * This descriptor is trusted Coordinator custody, never a remote proof DTO.
 * Each identity proof has the same 16..64-byte configured size. Both are
 * independently verified under the Owner callback gate, followed by the
 * Owner verify_session provider's exact candidate/key installation check. */
typedef struct ucn_i_security_reauth_call {
    const ucn_i_security_candidate_t *candidate, *previous;
    const uint8_t *local_record; /* Exactly UCN_I_SECURITY_RECORD_BYTES. */
    const ucn_i_security_durability_base_t *durability;
    const ucn_i_peer_reauth_record_t *record; /* Final stage 2 canonical Record208. */
    const uint8_t *challenge_proof, *response_proof;
    void *verify_context;
    ucn_i_security_reauth_verify_fn verify_identity;
    uint64_t started_us, parent_deadline_us, now_us;
    uint8_t initiator, challenge_proof_bytes, response_proof_bytes;
    uint8_t reserved_zero[5];
} ucn_i_security_reauth_call_t;
typedef struct ucn_i_security_reauth_workspace {
    ucn_i_sha256_workspace_t hash;
    uint8_t message[UCN_I_SECURITY_REAUTH_MESSAGE_BYTES];
    uint8_t digest[32];
    uint8_t proof[UCN_I_SECURITY_PROOF_BYTES];
} ucn_i_security_reauth_workspace_t;

/* EN: Context resolution may decode a complete C0 header and stage both
 * result DTOs.  Keep that bounded storage caller-owned so MCU task stacks do
 * not depend on the selected address width or security profile.
 * 中文：上下文解析可能需要解码完整 C0 头并暂存两个结果 DTO。该有界存储由
 * 调用方持有，避免 MCU 任务栈随地址宽度或安全 Profile 变化。 */
typedef struct ucn_i_security_c0_origin_resolve_workspace {
    ucn_i_c0_frame_t frame;
    ucn_i_security_control_access_request_t access;
    ucn_i_security_current_facts_t facts;
} ucn_i_security_c0_origin_resolve_workspace_t;

typedef struct ucn_i_security_packet_workspace {
    ucn_i_security_candidate_t candidate;
    ucn_i_security_c1_origin_material_t material;
    ucn_i_c0_frame_t control_frame;
    ucn_i_security_origin_protect_request_t origin_protect;
    ucn_i_security_origin_open_request_t origin_open;
    ucn_i_security_hop_request_t hop_protect;
    ucn_i_security_hop_verify_request_t hop_verify;
    ucn_i_security_replay_handle_t replay;
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES];
    uint8_t plaintext[UCN_ADAPTER_FRAME_BYTES];
    uint8_t origin_aad[UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES];
    uint8_t nonce_input[UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES];
    uint8_t hop_aad[UCN_I_SECURITY_HOP_AAD_BYTES];
    uint8_t digest[32];
    ucn_i_sha256_workspace_t digest_workspace;
    struct {
        size_t header_bytes;
        size_t packet_bytes;
        size_t packet_before_hop_bytes;
        size_t hop_trailer_bytes;
        size_t hop_aad_bytes;
        size_t offset;
        uint32_t hop_sequence;
        uint8_t provider_invoked;
        uint8_t reserved_zero[3];
    } packet_state;
} ucn_i_security_packet_workspace_t;

/* EN: Only Security can publish this fact after origin/hop verification and
 * replay reservation.  It is valid only while the exact replay reservation
 * remains pending and the caller-owned plaintext buffer is unchanged.
 * 中文：只有 Security 在 Origin/Hop 验证并预留 Replay 后才能发布本事实。它仅在
 * 精确 Replay 预留仍待提交且调用方 Plaintext 缓冲未改变期间有效。 */
typedef struct ucn_i_security_authenticated_control {
    ucn_i_security_handle_t session;
    ucn_i_security_replay_handle_t replay;
    const uint8_t *body;
    size_t body_bytes;
    uint64_t transaction_id;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint32_t source_address;
    uint32_t destination_address;
    uint32_t source_binding_generation;
    uint32_t destination_binding_generation;
    uint16_t security_owner_instance;
    uint16_t protocol_opcode;
    uint8_t body_digest[UCN_I_DIGEST128_BYTES];
    uint8_t traffic_class;
    uint8_t delivery_guarantee;
    uint8_t interaction_role;
    uint8_t payload_kind;
    uint8_t origin_security;
    uint8_t hop_limit;
    uint8_t direction;
    uint8_t reserved_zero;
} ucn_i_security_authenticated_control_t;

/* EN: This immutable relay fact exposes only canonical C0 routing metadata
 * and the still-protected origin bytes.  The source packet must remain
 * byte-identical until the hop replay obligation is committed or aborted.
 * 中文：该不可变中继事实只暴露规范 C0 路由元数据和仍受 Origin 保护的字节。
 * 在逐跳 Replay obligation 提交或回滚前，源 Packet 必须逐字节不变。 */
typedef struct ucn_i_security_c0_relay_fact {
    ucn_i_security_hop_replay_handle_t replay;
    const uint8_t *packet;
    size_t packet_bytes;
    uint64_t transaction_id;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint32_t source_address;
    uint32_t destination_address;
    uint32_t source_binding_generation;
    uint32_t destination_binding_generation;
    uint16_t protocol_opcode;
    uint8_t packet_digest[UCN_I_DIGEST128_BYTES];
    uint8_t traffic_class;
    uint8_t delivery_guarantee;
    uint8_t interaction_role;
    uint8_t payload_kind;
    uint8_t origin_security;
    uint8_t hop_limit;
    uint8_t reserved_zero[2];
} ucn_i_security_c0_relay_fact_t;

/* EN: A protected multi-hop C0 path has two independent security contexts.
 * origin_session authenticates the immutable end-to-end principal and body;
 * hop_session authenticates only the current physical neighbour.  They may
 * be the same only for the existing direct-link fast path.
 * 中文：受保护的多跳 C0 路径包含两个独立安全上下文。origin_session 认证
 * 不可变的端到端主体与正文；hop_session 只认证当前物理邻居。只有既有直达
 * 快速路径才允许二者相同。 */
typedef struct ucn_i_security_c0_path_context {
    ucn_i_security_handle_t origin_session;
    ucn_i_security_current_facts_t origin_facts;
    ucn_i_security_handle_t hop_session;
    ucn_i_security_current_facts_t hop_facts;
} ucn_i_security_c0_path_context_t;

typedef struct ucn_i_security_c0_path_workspace {
    ucn_i_security_packet_workspace_t packet;
    ucn_i_security_c0_relay_fact_t relay;
    uint8_t origin_packet[UCN_ADAPTER_FRAME_BYTES];
} ucn_i_security_c0_path_workspace_t;

typedef struct ucn_i_security_control_validate_workspace {
    ucn_i_c0_frame_t canonical;
    uint8_t digest[UCN_I_DIGEST128_BYTES];
    uint8_t aad[UCN_I_C0_ORIGIN_AAD_MAX_BYTES];
    ucn_i_sha256_workspace_t digest_workspace;
} ucn_i_security_control_validate_workspace_t;

/* EN: owner must point to fully zeroed caller-owned storage. Initialization
 * acquires config->state_lock and rejects every live/nonzero Owner without
 * modifying it; owner_destroy() returns the storage to the zero state.
 * 中文：owner 必须指向调用方持有且完整清零的存储。初始化会先取得
 * config->state_lock；任何仍存活或非零的 Owner 都会零写拒绝，
 * owner_destroy() 成功后才把该存储恢复为全零。 */
ucn_result_t ucn_i_security_owner_init(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_config_t *config);
ucn_result_t ucn_i_security_prepare_static_session(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_candidate_t *candidate,
    const uint8_t *proof,
    size_t proof_bytes,
    const ucn_i_security_durability_base_t *durability,
    uint64_t now_us,
    ucn_i_security_handle_t *handle_out);
/* Signed max-history successor only; generic static prepare remains prior+1.
 * The caller retains immutable inputs and exclusive workspace through all
 * callbacks. Workspace/output are disjoint from one another, the Owner and
 * every borrowed input; inputs must also be outside the Owner. */
ucn_result_t ucn_i_security_prepare_reauth_session(ucn_i_security_owner_t *owner,
    const ucn_i_security_reauth_call_t *call, ucn_i_security_reauth_workspace_t *workspace,
    ucn_i_security_handle_t *handle_out);
ucn_result_t ucn_i_security_peer_is_fenced(ucn_i_security_owner_t *owner,
    const ucn_i_security_binding_t *local, const ucn_i_security_binding_t *peer, bool *out);
/* Trusted product policy, not network authentication: bind immutable borrowed
 * exact rules once before activation. Rules remain alive until Session retirement.
 * Explicit rules replace, never union with, the Owner's default ACL. */
ucn_result_t ucn_i_security_bind_session_acl(ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session, const ucn_i_security_acl_rule_t *rules,
    uint16_t count);
/* Locked range-only preflight for Coordinator output. Protect every occupied
 * Session's borrowed ACL, including pending/fenced successors after handoff,
 * plus default ACL/domain tables and Owner storage. No internal pointer escapes.
 * Rust's immutable lifetime borrowing prevents the equivalent mutable alias. */
ucn_result_t ucn_i_security_borrowed_output_check(ucn_i_security_owner_t *owner,
    const void *output, size_t bytes);
/* Locked ACTIVE snapshot, including exact selectors/transcript/expiry. */
ucn_result_t ucn_i_security_session_candidate_get(ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session, ucn_i_security_candidate_t *candidate_out);
/* Local H1 C0 retry only: immutable Origin prefix, fresh shared Hop sequence.
 * Trusted Coordinator exclusively owns the original protected frame for this
 * exact Session; this is not authentication of arbitrary caller bytes and does
 * not verify the old tag. Receiver must independently authenticate Origin.
 * Input/output/workspace must be disjoint; no Origin reseal or nonce reuse. */
ucn_result_t ucn_i_security_c0_refresh_hop(ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session, const ucn_i_security_current_facts_t *facts,
    const uint8_t *packet, size_t packet_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output, size_t capacity, size_t *written);

ucn_result_t ucn_i_security_requirement_get(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle,
    ucn_i_security_requirement_view_t *requirement_out);
ucn_result_t ucn_i_security_bind_persistence(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle,
    ucn_handle_t persistence_handle,
    const uint8_t expected_published_digest[16]);
ucn_result_t ucn_i_security_activate(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle,
    const ucn_i_security_durability_proof_t *proof,
    const ucn_i_security_current_facts_t *facts);
ucn_result_t ucn_i_security_authorize(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_access_request_t *request,
    const ucn_i_security_current_facts_t *facts);
ucn_result_t ucn_i_security_c1_origin_aad_build(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c1_origin_material_t *material,
    uint8_t output[UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES]);
ucn_result_t ucn_i_security_sequence_nonce_input_build(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c1_origin_material_t *material,
    uint8_t output[UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES]);
ucn_result_t ucn_i_security_tx_reserve(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_access_request_t *request,
    const ucn_i_security_current_facts_t *facts,
    ucn_i_security_tx_handle_t *handle_out);
ucn_result_t ucn_i_security_tx_commit(
    ucn_i_security_owner_t *owner,
    ucn_i_security_tx_handle_t handle,
    const ucn_i_security_current_facts_t *facts);
ucn_result_t ucn_i_security_tx_abort(
    ucn_i_security_owner_t *owner,
    ucn_i_security_tx_handle_t handle);
/* Direct synchronous layer SPI avoids a forwarding frame in bounded actors.
 * origin_only selects the existing separate Origin layer, never O0 fallback.
 * 直接同步分层入口，省去转发调用帧；仅选原Origin层，不是降级开关。 */
ucn_result_t ucn_i_security_c1_seal_layer(bool origin_only,
    ucn_i_security_owner_t *, ucn_i_security_tx_handle_t,
    const ucn_i_security_c1_origin_material_t *, const uint8_t *,
    ucn_i_security_packet_workspace_t *, uint8_t *, size_t, size_t *);
ucn_result_t ucn_i_security_c1_protect(
    ucn_i_security_owner_t *owner,
    ucn_i_security_tx_handle_t tx,
    const ucn_i_security_c1_origin_material_t *material,
    const uint8_t *plaintext,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes);
/* Borrowed synchronous C1 call descriptor. Caller keeps it immutable until
 * return; it may share storage with a mutually exclusive send material, not
 * with packet/workspace/outputs. No descriptor pointer escapes this call.
 * 同步借用描述符，调用返回前保持不变，可与互斥发送阶段共用静态union。 */
typedef struct ucn_i_security_c1_open_call {
    ucn_i_security_handle_t session;
    const ucn_i_security_current_facts_t *facts;
    const ucn_i_security_access_request_t *access;
    const uint8_t *packet;
    size_t packet_bytes;
    uint8_t *plaintext_output;
    size_t plaintext_capacity;
    size_t *plaintext_bytes;
    ucn_i_security_replay_handle_t *replay_out;
    uint8_t origin_only;
} ucn_i_security_c1_open_call_t;
ucn_result_t ucn_i_security_c1_open_borrowed(
    ucn_i_security_owner_t *, const ucn_i_security_c1_open_call_t *,
    ucn_i_security_packet_workspace_t *);
ucn_result_t ucn_i_security_c1_open(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session,
    const ucn_i_security_current_facts_t *facts,
    const ucn_i_security_access_request_t *access,
    const uint8_t *packet,
    size_t packet_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *plaintext_output,
    size_t plaintext_capacity,
    size_t *plaintext_bytes,
    ucn_i_security_replay_handle_t *replay_out);
/* EN: Project the actual H0/H1 Session onto its Origin layer. Neither function
 * authenticates a physical neighbour or consumes Hop counters/replay. The
 * Coordinator must separately establish the current Hop before app delivery.
 * ZH: 将真实H0/H1 Session投影到Origin层；不认证物理邻居、不消耗Hop序号或Replay。
 * 协调器须在业务交付前另行完成当前Hop验证，禁止伪造第二个同Peer活动Session。 */
ucn_result_t ucn_i_security_c1_origin_protect(
    ucn_i_security_owner_t *owner,
    ucn_i_security_tx_handle_t tx,
    const ucn_i_security_c1_origin_material_t *material,
    const uint8_t *plaintext,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes);
ucn_result_t ucn_i_security_c1_origin_open(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session,
    const ucn_i_security_current_facts_t *facts,
    const ucn_i_security_access_request_t *access,
    const uint8_t *packet,
    size_t packet_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *plaintext_output,
    size_t plaintext_capacity,
    size_t *plaintext_bytes,
    ucn_i_security_replay_handle_t *replay_out);
/* EN: Borrowed descriptor for the outlined RX phase; it must not alias any
 * input, output, Owner or workspace. The original convenience API is retained.
 * ZH: 独立RX阶段借用描述符，禁止与输入/输出/Owner/工作区别名；保留原便捷API。 */
typedef struct ucn_i_security_c0_open_call {
    const ucn_i_security_control_access_request_t *access;
    const ucn_i_security_current_facts_t *facts;
    const uint8_t *packet;
    size_t packet_bytes;
    uint8_t *plaintext_output;
    size_t plaintext_capacity;
    size_t *plaintext_bytes;
    ucn_i_security_authenticated_control_t *fact_out;
} ucn_i_security_c0_open_call_t;
ucn_result_t ucn_i_security_c0_open_borrowed(ucn_i_security_owner_t *,
    const ucn_i_security_c0_open_call_t *, ucn_i_security_packet_workspace_t *);
ucn_result_t ucn_i_security_c0_open(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_control_access_request_t *access,
    const ucn_i_security_current_facts_t *facts,
    const uint8_t *packet,
    size_t packet_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *plaintext_output,
    size_t plaintext_capacity,
    size_t *plaintext_bytes,
    ucn_i_security_authenticated_control_t *fact_out);
/* Side-effect-free exact C0 ACL preflight, including current ACTIVE facts.
 * Neither authenticates a packet nor consumes any sequence/replay state. */
ucn_result_t ucn_i_security_control_authorize(ucn_i_security_owner_t *owner,
    const ucn_i_security_control_access_request_t *request,
    const ucn_i_security_current_facts_t *facts);
ucn_result_t ucn_i_security_c0_hop_open(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t inbound_session,
    const ucn_i_security_current_facts_t *facts,
    const uint8_t *packet,
    size_t packet_bytes,
    ucn_i_security_c0_path_workspace_t *workspace);
/* C1 independent Hop layer. wrap consumes an already protected Origin packet;
 * open verifies only the physical neighbor and reserves its Hop replay. The
 * trusted Coordinator must commit/abort replay and independently open Origin
 * at the destination. Relay TTL adjustment precedes wrap, after verified Hop.
 * No C0 semantics or endpoint identity are inferred from a Hop Session. */
ucn_result_t ucn_i_security_c1_hop_wrap(ucn_i_security_owner_t *,
    ucn_i_security_handle_t, const ucn_i_security_current_facts_t *,
    const uint8_t *, size_t, ucn_i_security_packet_workspace_t *,
    uint8_t *, size_t, size_t *);
ucn_result_t ucn_i_security_c1_hop_open(ucn_i_security_owner_t *,
    ucn_i_security_handle_t, const ucn_i_security_current_facts_t *,
    const uint8_t *, size_t, ucn_i_security_packet_workspace_t *,
    ucn_i_security_hop_replay_handle_t *);
/* Exact immutable Session context check, not a generic ACL fingerprint match.
 * At least one fingerprint is required; NULL selects no check for that layer.
 * No counters, callbacks, or outputs are changed. */
ucn_result_t ucn_i_security_c1_context_check(ucn_i_security_owner_t *,
    const ucn_i_security_handle_t *, const ucn_i_security_current_facts_t *,
    const uint8_t *origin_fingerprint, const uint8_t *hop_fingerprint);
ucn_result_t ucn_i_security_c0_hop_seal(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t outbound_session,
    const ucn_i_security_current_facts_t *facts,
    const ucn_i_security_c0_relay_fact_t *relay,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes);
/* Split multi-hop only: Origin Session MUST be H0; Hop Session MUST be H1.
 * Direct H1 uses c0_protect/open and its single combined replay handle.
 * H1 Origin misuse rejects before nonce/replay reservation or workspace writes. */
ucn_result_t ucn_i_security_c0_path_protect(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c0_protect_request_t *origin_request,
    ucn_i_security_handle_t hop_session,
    const ucn_i_security_current_facts_t *hop_facts,
    const uint8_t *plaintext,
    size_t plaintext_bytes,
    ucn_i_security_c0_path_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes);
ucn_result_t ucn_i_security_c0_path_open(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_control_access_request_t *origin_access,
    const ucn_i_security_current_facts_t *origin_facts,
    ucn_i_security_handle_t hop_session,
    const ucn_i_security_current_facts_t *hop_facts,
    const uint8_t *packet,
    size_t packet_bytes,
    ucn_i_security_c0_path_workspace_t *workspace,
    uint8_t *plaintext_output,
    size_t plaintext_capacity,
    size_t *plaintext_bytes,
    ucn_i_security_authenticated_control_t *fact_out,
    ucn_i_security_hop_replay_handle_t *hop_replay_out);
ucn_result_t ucn_i_security_c0_hop_receive_context_resolve(
    ucn_i_security_owner_t *owner,
    const uint8_t *packet,
    size_t packet_bytes,
    uint64_t now_us,
    uint32_t link_generation,
    ucn_i_security_handle_t *session_out,
    ucn_i_security_current_facts_t *facts_out);
ucn_result_t ucn_i_security_c0_hop_send_context_resolve(
    ucn_i_security_owner_t *owner,
    uint16_t protocol_opcode,
    uint64_t now_us,
    uint32_t link_generation,
    ucn_i_security_handle_t *session_out,
    ucn_i_security_current_facts_t *facts_out);
ucn_result_t ucn_i_security_c0_origin_receive_context_resolve(
    ucn_i_security_owner_t *owner,
    const uint8_t *packet,
    size_t packet_bytes,
    uint64_t now_us,
    ucn_i_security_c0_origin_resolve_workspace_t *workspace,
    ucn_i_security_control_access_request_t *access_out,
    ucn_i_security_current_facts_t *facts_out);
ucn_result_t ucn_i_security_hop_replay_commit(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_hop_replay_handle_t *handle,
    const ucn_i_security_current_facts_t *facts);
ucn_result_t ucn_i_security_hop_replay_abort(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_hop_replay_handle_t *handle);
/* EN: Adapter ingress never manufactures a Session handle or ACL request.
 * Security resolves one and only one active Session from the canonical C0
 * addresses, Bindings, protection profile, link generation and exact inbound
 * Opcode before the provider is invoked.  No match or an ambiguous match is
 * fail-closed and leaves both outputs untouched.
 * 中文：Adapter 入站不得自行拼装 Session Handle 或 ACL 请求。Security 必须先
 * 根据 canonical C0 地址、Binding、保护 Profile、Link Generation 与精确入站
 * Opcode 唯一解析出一个活动 Session，之后才允许调用 Provider。无匹配或多重
 * 匹配均失败关闭，两个输出保持不变。 */
ucn_result_t ucn_i_security_c0_receive_context_resolve(
    ucn_i_security_owner_t *owner,
    const uint8_t *packet,
    size_t packet_bytes,
    uint64_t now_us,
    uint32_t link_generation,
    ucn_i_security_control_access_request_t *access_out,
    ucn_i_security_current_facts_t *facts_out);
ucn_result_t ucn_i_security_c0_protect(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c0_protect_request_t *request,
    const uint8_t *plaintext,
    size_t plaintext_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes);
ucn_result_t ucn_i_security_control_fact_validate(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_authenticated_control_t *fact,
    const ucn_i_security_current_facts_t *current_facts,
    ucn_i_security_control_validate_workspace_t *workspace);
ucn_result_t ucn_i_security_replay_reserve(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session,
    const ucn_i_security_current_facts_t *facts,
    uint64_t sequence,
    const uint8_t aad_digest[16],
    const uint8_t payload_digest[16],
    ucn_i_security_replay_handle_t *handle_out);
ucn_result_t ucn_i_security_replay_commit(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_replay_handle_t *handle,
    const ucn_i_security_current_facts_t *facts);
ucn_result_t ucn_i_security_control_replay_complete(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_replay_handle_t *handle,
    const ucn_i_security_current_facts_t *facts);
ucn_result_t ucn_i_security_control_replay_commit(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_replay_handle_t *handle,
    const ucn_i_security_current_facts_t *facts);
ucn_result_t ucn_i_security_replay_abort(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_replay_handle_t *handle);
ucn_result_t ucn_i_security_replay_maintain(
    ucn_i_security_owner_t *owner,
    uint64_t now_us,
    uint16_t budget,
    uint16_t *inspected_out,
    uint16_t *expired_out);
ucn_result_t ucn_i_security_session_get(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle,
    ucn_i_security_session_view_t *view_out);
ucn_result_t ucn_i_security_fence(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle);
ucn_result_t ucn_i_security_retire_fenced(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle);
ucn_result_t ucn_i_security_owner_destroy(ucn_i_security_owner_t *owner);

ucn_result_t ucn_i_security_record_encode(
    uint32_t realm_id,
    const ucn_i_security_candidate_t *candidate,
    uint8_t output[UCN_I_SECURITY_RECORD_BYTES]);
ucn_result_t ucn_i_security_record_decode(
    const uint8_t input[UCN_I_SECURITY_RECORD_BYTES],
    ucn_i_security_codec_workspace_t *workspace,
    uint32_t *realm_id_out,
    ucn_i_security_candidate_t *candidate_out);

#endif
