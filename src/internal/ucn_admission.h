#ifndef UCN_INTERNAL_ADMISSION_H
#define UCN_INTERNAL_ADMISSION_H

#include "internal/ucn_owner.h"
#include "internal/ucn_identity.h"
#include "ucn/ucn_config.h"

#define UCN_I_ADMISSION_SCHEMA UINT16_C(1)
#define UCN_I_ADMISSION_COOKIE_BYTES 16U
#define UCN_I_ADMISSION_EVIDENCE_BYTES 128U
#define UCN_I_ADMISSION_PRINCIPAL_BYTES 16U
#define UCN_I_ADMISSION_DIGEST_BYTES 32U
#define UCN_I_ADMISSION_HELLO_BYTES 40U
#define UCN_I_ADMISSION_COOKIE_CHALLENGE_BYTES 40U
#define UCN_I_ADMISSION_HELLO_COOKIE_FIXED_BYTES 82U
#define UCN_I_ADMISSION_HELLO_COOKIE_MAX_BYTES \
    (UCN_I_ADMISSION_HELLO_COOKIE_FIXED_BYTES + UCN_I_ADMISSION_COOKIE_BYTES)

#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_I_ADMISSION_PENDING_COUNT 2U
#define UCN_I_ADMISSION_MAX_PENDING_PER_LINK 1U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_I_ADMISSION_PENDING_COUNT 4U
#define UCN_I_ADMISSION_MAX_PENDING_PER_LINK 2U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define UCN_I_ADMISSION_PENDING_COUNT 8U
#define UCN_I_ADMISSION_MAX_PENDING_PER_LINK 2U
#else
#error "UCN_PROFILE must be UCN_PROFILE_NANO, UCN_PROFILE_LITE, or UCN_PROFILE_FULL"
#endif

typedef uint8_t ucn_i_admission_phase_t;
enum {
    UCN_I_ADMISSION_EMPTY = 0,
    UCN_I_ADMISSION_COOKIE_VERIFIED = 1,
    UCN_I_ADMISSION_AUTHORITY_VERIFIED = 2,
    UCN_I_ADMISSION_DEVICE_VERIFIED = 3,
    UCN_I_ADMISSION_ADDRESS_OFFERED = 4,
    UCN_I_ADMISSION_DEVICE_COMMITTED = 5,
    UCN_I_ADMISSION_FINAL_DURABLE = 6,
    UCN_I_ADMISSION_ABORTED = 7
};

typedef uint8_t ucn_i_admission_event_t;
enum {
    UCN_I_ADMISSION_EVENT_AUTHORITY_PROOF = 1,
    UCN_I_ADMISSION_EVENT_DEVICE_PROOF = 2,
    UCN_I_ADMISSION_EVENT_ADDRESS_OFFER = 3,
    UCN_I_ADMISSION_EVENT_DEVICE_COMMIT = 4,
    UCN_I_ADMISSION_EVENT_FINAL_DURABLE = 5,
    UCN_I_ADMISSION_EVENT_ABORT = 6
};

typedef struct ucn_i_admission_link {
    uint32_t generation;
    uint16_t id;
    uint16_t reserved_zero;
} ucn_i_admission_link_t;

typedef struct ucn_i_admission_key {
    ucn_i_admission_link_t link;
    uint64_t transaction_id;
    uint32_t local_peer_discriminator;
    uint8_t identity_digest[UCN_I_ADMISSION_PRINCIPAL_BYTES];
} ucn_i_admission_key_t;

typedef struct ucn_i_admission_hello {
    uint64_t device_nonce;
    uint64_t transaction_id;
    uint8_t identity_digest[UCN_I_ADMISSION_PRINCIPAL_BYTES];
} ucn_i_admission_hello_t;

typedef struct ucn_i_admission_cookie_challenge {
    uint64_t transaction_id;
    uint32_t cookie_time_bucket;
    uint8_t cookie[UCN_I_ADMISSION_COOKIE_BYTES];
    uint8_t cookie_bytes;
    uint8_t reserved_zero[3];
} ucn_i_admission_cookie_challenge_t;

typedef struct ucn_i_admission_hello_cookie {
    ucn_i_admission_key_t key;
    uint64_t device_nonce;
    uint64_t lease_freshness_challenge_nonce;
    uint8_t prior_messages_hash[UCN_I_ADMISSION_DIGEST_BYTES];
    uint8_t cookie[UCN_I_ADMISSION_COOKIE_BYTES];
    uint8_t cookie_bytes;
    uint8_t reserved_zero[3];
} ucn_i_admission_hello_cookie_t;

typedef struct ucn_i_admission_evidence {
    uint8_t bytes[UCN_I_ADMISSION_EVIDENCE_BYTES];
    uint8_t length;
    uint8_t reserved_zero[7];
} ucn_i_admission_evidence_t;

/* EN: This is the typed semantic transcript. It is never a C struct Wire
 * image; codecs serialize fields explicitly. Each phase may populate only its
 * own group, and every earlier group becomes immutable.
 * 中文：这是有类型的语义 Transcript，不是把 C 结构体直接发到线上。Codec 必须
 * 逐字段序列化；每个阶段只能填充本阶段字段，已完成阶段的字段随后不可变。 */
typedef struct ucn_i_admission_transcript {
    uint8_t joining_device_principal[UCN_I_ADMISSION_PRINCIPAL_BYTES];
    uint8_t joining_device_identity_digest[UCN_I_ADMISSION_PRINCIPAL_BYTES];
    uint8_t authority_principal[UCN_I_ADMISSION_PRINCIPAL_BYTES];
    uint8_t binding_lease_id[16];
    uint8_t durable_fence_token[16];
    uint8_t allocation_high_water_digest[16];
    uint8_t quorum_config_digest[32];
    uint8_t signer_set_digest[32];
    uint8_t threshold_proof_digest[32];
    uint8_t freshness_proof_transcript_hash[32];
    uint8_t prior_messages_hash[32];
    uint64_t device_nonce;
    uint64_t authority_nonce;
    uint64_t transaction_id;
    uint64_t lease_freshness_challenge_nonce;
    uint64_t binding_lease_duration_us;
    uint64_t authority_lease_sequence;
    uint64_t authority_lease_duration_us;
    uint64_t freshness_max_remaining_lease_us;
    uint32_t authority_generation;
    uint32_t realm_id;
    uint32_t proposed_address;
    uint32_t address_binding_generation;
    uint32_t authority_address;
    uint32_t authority_binding_generation;
    uint32_t selected_hop_key_generation;
    uint32_t selected_e2e_key_generation;
    uint32_t selected_session_generation;
    uint32_t selected_link_generation;
    uint16_t bootstrap_header_contract;
    uint16_t selected_link_id;
    uint16_t authority_signer_count;
    uint16_t authority_quorum_threshold;
    uint16_t selected_hop_key_id;
    uint16_t selected_e2e_key_id;
    uint8_t protocol_version;
    uint8_t binding_mode;
    uint8_t selected_hop_suite;
    uint8_t selected_e2e_mode;
    uint8_t selected_e2e_suite;
    uint8_t reserved_zero[3];
} ucn_i_admission_transcript_t;

/* EN: Identity/Persistence produce this immutable proof-backed view. Admission
 * validates it but never calls either Owner directly.
 * 中文：该不可变 View 由 Identity/Persistence 的证明链形成。Admission 只验证
 * View，不得直接调用任一 Owner。 */
typedef ucn_i_identity_binding_view_t ucn_i_admission_binding_view_t;

typedef struct ucn_i_admission_handle {
    uint32_t runtime_instance;
    uint32_t slot_generation;
    uint64_t transaction_id;
    uint16_t owner_instance;
    uint16_t slot;
} ucn_i_admission_handle_t;

typedef struct ucn_i_admitted_view {
    ucn_i_admission_binding_view_t binding;
    ucn_i_admission_link_t link;
    uint32_t session_generation;
    uint32_t admission_generation;
} ucn_i_admitted_view_t;

/* EN: Final Admission exports exactly one immutable requirement for the
 * Coordinator to route to Security. It does not contain a Security Owner
 * pointer or key material; Security resolves the authenticated selectors and
 * transcript proof through its own Provider.
 * 中文：Admission 终态只导出这一份不可变 Requirement，由 Coordinator 路由给
 * Security。它不携带 Security Owner 指针或密钥材料；认证 selector 与
 * transcript proof 仍由 Security 通过自己的 Provider 解析。 */
typedef struct ucn_i_admission_session_requirement {
    ucn_i_admitted_view_t admitted;
    uint64_t transaction_id;
    uint64_t absolute_deadline_us;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint32_t authority_address;
    uint32_t authority_binding_generation;
    uint32_t authority_generation;
    uint32_t hop_key_generation;
    uint32_t e2e_key_generation;
    uint16_t admission_owner_instance;
    uint16_t identity_owner_instance;
    uint16_t persistence_owner_instance;
    uint16_t hop_key_id;
    uint16_t e2e_key_id;
    uint8_t hop_suite;
    uint8_t e2e_mode;
    uint8_t e2e_suite;
    uint8_t reserved_zero;
    uint8_t authority_principal[UCN_I_ADMISSION_PRINCIPAL_BYTES];
    uint8_t authority_freshness_transcript_hash[
        UCN_I_ADMISSION_DIGEST_BYTES];
} ucn_i_admission_session_requirement_t;

typedef struct ucn_i_admission_pending_view {
    ucn_i_admission_key_t key;
    uint64_t challenge_started_local_us;
    uint64_t deadline_us;
    uint8_t phase;
    uint8_t reserved_zero[7];
} ucn_i_admission_pending_view_t;

typedef ucn_result_t (*ucn_i_admission_issue_cookie_fn)(
    void *context,
    const ucn_i_admission_hello_t *hello,
    ucn_i_admission_link_t link,
    uint32_t cookie_time_bucket,
    uint8_t output[UCN_I_ADMISSION_COOKIE_BYTES],
    uint8_t *output_bytes);
typedef ucn_result_t (*ucn_i_admission_verify_cookie_fn)(
    void *context,
    const ucn_i_admission_hello_cookie_t *hello_cookie);
typedef ucn_result_t (*ucn_i_admission_authorize_event_fn)(
    void *context,
    ucn_i_admission_event_t event,
    const ucn_i_admission_key_t *key,
    const ucn_i_admission_transcript_t *transcript,
    uint64_t now_us,
    const ucn_i_admission_evidence_t *evidence);

typedef struct ucn_i_admission_provider {
    uint16_t struct_size;
    uint16_t api_version;
    void *context;
    ucn_i_admission_issue_cookie_fn issue_cookie;
    ucn_i_admission_verify_cookie_fn verify_cookie;
    ucn_i_admission_authorize_event_fn authorize_event;
} ucn_i_admission_provider_t;

typedef struct ucn_i_admission_config {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint16_t owner_instance;
    uint16_t identity_owner_instance;
    uint16_t persistence_owner_instance;
    uint8_t max_pending_per_link;
    uint8_t token_burst;
    uint8_t tokens_per_second;
    uint8_t reserved_zero;
    uint64_t pending_timeout_us;
    ucn_i_admission_provider_t provider;
    ucn_i_lock_ops_t state_lock;
    ucn_i_callback_gate_t *provider_gate;
} ucn_i_admission_config_t;

typedef struct ucn_i_admission_link_budget {
    ucn_i_admission_link_t link;
    uint64_t last_refill_us;
    uint8_t tokens;
    uint8_t occupied;
    uint8_t reserved_zero[6];
} ucn_i_admission_link_budget_t;

typedef struct ucn_i_admission_slot {
    ucn_i_admission_key_t key;
    ucn_i_admission_transcript_t transcript;
    ucn_i_admitted_view_t admitted;
    uint64_t challenge_started_local_us;
    uint64_t deadline_us;
    uint32_t slot_generation;
    uint8_t occupied;
    uint8_t phase;
    uint8_t reserved_zero[2];
} ucn_i_admission_slot_t;

typedef struct ucn_i_admission_owner {
    uint32_t magic;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint32_t next_operation_id;
    uint16_t schema;
    uint16_t owner_instance;
    uint16_t identity_owner_instance;
    uint16_t persistence_owner_instance;
    uint16_t maintenance_cursor;
    uint8_t max_pending_per_link;
    uint8_t token_burst;
    uint8_t tokens_per_second;
    uint8_t callback_active;
    uint64_t pending_timeout_us;
    ucn_i_admission_provider_t provider;
    ucn_i_lock_ops_t state_lock;
    ucn_i_callback_gate_t *provider_gate;
    ucn_i_callback_claim_t callback_claim;
    ucn_i_admission_hello_t callback_hello;
    ucn_i_admission_hello_cookie_t callback_hello_cookie;
    ucn_i_admission_key_t callback_key;
    ucn_i_admission_transcript_t callback_transcript;
    ucn_i_admission_evidence_t callback_evidence;
    ucn_i_admission_binding_view_t callback_binding;
    uint64_t callback_now_us;
    uint32_t callback_cookie_bucket;
    uint8_t callback_cookie[UCN_I_ADMISSION_COOKIE_BYTES];
    uint8_t callback_cookie_bytes;
    uint8_t callback_event;
    uint8_t reserved_zero[2];
    ucn_i_admission_link_budget_t budgets[UCN_LINK_COUNT];
    ucn_i_admission_slot_t pending[UCN_I_ADMISSION_PENDING_COUNT];
} ucn_i_admission_owner_t;

ucn_result_t ucn_i_admission_hello_encode(
    const ucn_i_admission_hello_t *hello,
    uint8_t output[UCN_I_ADMISSION_HELLO_BYTES]);
ucn_result_t ucn_i_admission_hello_decode(
    const uint8_t input[UCN_I_ADMISSION_HELLO_BYTES],
    ucn_i_admission_hello_t *hello_out);
ucn_result_t ucn_i_admission_cookie_challenge_encode(
    const ucn_i_admission_cookie_challenge_t *challenge,
    uint8_t output[UCN_I_ADMISSION_COOKIE_CHALLENGE_BYTES]);
ucn_result_t ucn_i_admission_cookie_challenge_decode(
    const uint8_t input[UCN_I_ADMISSION_COOKIE_CHALLENGE_BYTES],
    ucn_i_admission_cookie_challenge_t *challenge_out);
ucn_result_t ucn_i_admission_hello_cookie_encode(
    const ucn_i_admission_hello_cookie_t *hello_cookie,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes);
ucn_result_t ucn_i_admission_hello_cookie_decode(
    const uint8_t *input,
    size_t input_bytes,
    uint32_t local_peer_discriminator,
    ucn_i_admission_hello_cookie_t *hello_cookie_out);

ucn_result_t ucn_i_admission_owner_init(
    ucn_i_admission_owner_t *owner,
    const ucn_i_admission_config_t *config);
ucn_result_t ucn_i_admission_issue_cookie(
    ucn_i_admission_owner_t *owner,
    const ucn_i_admission_hello_t *hello,
    ucn_i_admission_link_t link,
    uint64_t now_us,
    uint32_t request_bytes,
    uint32_t response_bytes,
    uint32_t cookie_time_bucket,
    ucn_i_admission_cookie_challenge_t *challenge_out);
ucn_result_t ucn_i_admission_open_after_cookie(
    ucn_i_admission_owner_t *owner,
    const ucn_i_admission_hello_cookie_t *hello_cookie,
    const ucn_i_admission_transcript_t *transcript,
    uint64_t now_us,
    ucn_i_admission_handle_t *handle_out);
ucn_result_t ucn_i_admission_advance(
    ucn_i_admission_owner_t *owner,
    ucn_i_admission_handle_t handle,
    ucn_i_admission_event_t event,
    const ucn_i_admission_transcript_t *transcript,
    const ucn_i_admission_evidence_t *evidence,
    uint64_t now_us);
ucn_result_t ucn_i_admission_finalize(
    ucn_i_admission_owner_t *owner,
    ucn_i_admission_handle_t handle,
    const ucn_i_admission_transcript_t *transcript,
    const ucn_i_admission_evidence_t *evidence,
    const ucn_i_admission_binding_view_t *binding,
    uint64_t now_us,
    ucn_i_admitted_view_t *admitted_out);
ucn_result_t ucn_i_admission_pending_get(
    ucn_i_admission_owner_t *owner,
    ucn_i_admission_handle_t handle,
    ucn_i_admission_pending_view_t *view_out);
ucn_result_t ucn_i_admission_admitted_get(
    ucn_i_admission_owner_t *owner,
    ucn_i_admission_handle_t handle,
    uint64_t now_us,
    ucn_i_admitted_view_t *view_out);
ucn_result_t ucn_i_admission_session_requirement_get(
    ucn_i_admission_owner_t *owner,
    ucn_i_admission_handle_t handle,
    uint64_t now_us,
    ucn_i_admission_session_requirement_t *requirement_out);
ucn_result_t ucn_i_admission_expire(
    ucn_i_admission_owner_t *owner,
    uint64_t now_us,
    uint16_t budget,
    uint16_t *inspected_out,
    uint16_t *expired_out);
ucn_result_t ucn_i_admission_retire(
    ucn_i_admission_owner_t *owner,
    ucn_i_admission_handle_t handle);
ucn_result_t ucn_i_admission_owner_destroy(
    ucn_i_admission_owner_t *owner);

#endif
