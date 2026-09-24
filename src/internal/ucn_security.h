#ifndef UCN_INTERNAL_SECURITY_H
#define UCN_INTERNAL_SECURITY_H

#include "internal/ucn_owner.h"
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
#define UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES 72U
#define UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES 37U
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

typedef struct ucn_i_security_current_facts {
    ucn_i_security_binding_t local;
    ucn_i_security_binding_t peer;
    uint64_t now_us;
    uint32_t link_generation;
    uint32_t policy_generation;
} ucn_i_security_current_facts_t;

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
    uint8_t reserved_zero[3];
} ucn_i_security_replay_handle_t;

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
    uint8_t reserved_zero[3];
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
    ucn_i_security_candidate_t candidate;
    ucn_i_security_durability_base_t durability;
    uint8_t body[UCN_I_SECURITY_RECORD_BYTES];
    uint64_t transition_fingerprint;
    ucn_handle_t persistence_handle;
    uint8_t expected_published_digest[16];
    ucn_i_security_replay_window_t origin_replay;
    ucn_i_security_replay_window_t hop_replay;
    ucn_i_security_tx_reservation_t origin_tx_reservation;
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

typedef struct ucn_i_security_packet_workspace {
    ucn_i_security_candidate_t candidate;
    ucn_i_security_c1_origin_material_t material;
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
    struct {
        size_t header_bytes;
        size_t packet_bytes;
        size_t packet_before_hop_bytes;
        size_t hop_trailer_bytes;
        size_t offset;
        uint32_t hop_sequence;
        uint8_t provider_invoked;
        uint8_t reserved_zero[3];
    } protect_control;
} ucn_i_security_packet_workspace_t;

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
ucn_result_t ucn_i_security_c1_protect(
    ucn_i_security_owner_t *owner,
    ucn_i_security_tx_handle_t tx,
    const ucn_i_security_c1_origin_material_t *material,
    const uint8_t *plaintext,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes);
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
    ucn_i_security_replay_handle_t handle,
    const ucn_i_security_current_facts_t *facts);
ucn_result_t ucn_i_security_replay_abort(
    ucn_i_security_owner_t *owner,
    ucn_i_security_replay_handle_t handle);
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
