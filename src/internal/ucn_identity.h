#ifndef UCN_INTERNAL_IDENTITY_H
#define UCN_INTERNAL_IDENTITY_H

#include "internal/ucn_owner.h"
#include "ucn/ucn_config.h"

#define UCN_I_IDENTITY_SCHEMA UINT16_C(1)
#define UCN_I_IDENTITY_BINDING_RECORD_SCHEMA UINT16_C(1)
#define UCN_I_IDENTITY_AUTHORITY_SCHEMA_ID UINT16_C(0x4901)
#define UCN_I_IDENTITY_BINDING_SCHEMA_ID UINT16_C(0x4902)
#define UCN_I_IDENTITY_BINDING_OPERATION_KIND UINT16_C(0x0602)
#define UCN_I_IDENTITY_BINDING_RECORD_BYTES 96U
#define UCN_I_IDENTITY_PRINCIPAL_BYTES 16U
#define UCN_I_IDENTITY_LEASE_ID_BYTES 16U
#define UCN_I_IDENTITY_PERSISTENCE_REQUEST_FAILED 4U

typedef uint8_t ucn_i_identity_address_mode_t;
enum {
    UCN_I_IDENTITY_ADDRESS_STATIC = 1,
    UCN_I_IDENTITY_ADDRESS_LEASED = 2,
    UCN_I_IDENTITY_ADDRESS_SELF_PROPOSED = 3
};

typedef uint8_t ucn_i_identity_binding_phase_t;
enum {
    UCN_I_IDENTITY_BINDING_EMPTY = 0,
    UCN_I_IDENTITY_BINDING_AWAITING_DURABILITY = 1,
    UCN_I_IDENTITY_BINDING_ACTIVE = 2,
    UCN_I_IDENTITY_BINDING_FENCED = 3
};

/* EN: This is an immutable Coordinator-routed projection of the current
 * durable Address Authority. Identity consumes the view but never calls the
 * Authority or Persistence Owner directly.
 * 中文：这是 Coordinator 路由的当前 durable Address Authority 不可变投影。
 * Identity 只消费 View，不直接调用 Authority 或 Persistence Owner。 */
typedef struct ucn_i_identity_authority_view {
    uint64_t lease_sequence;
    uint64_t local_deadline_us;
    uint64_t record_generation;
    uint64_t foundation_transaction_id;
    uint64_t witness_generation;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint32_t authority_generation;
    uint16_t authority_owner_instance;
    uint16_t persistence_owner_instance;
    uint16_t schema_id;
    uint16_t schema_version;
    uint8_t principal[UCN_I_IDENTITY_PRINCIPAL_BYTES];
    uint8_t body_digest[16];
} ucn_i_identity_authority_view_t;

/* EN: Admission produces this immutable issue request after mutual identity
 * verification. The challenge start is captured by Admission and is the only
 * legal origin for the local binding deadline.
 * 中文：Admission 完成双向身份认证后产生该不可变签发请求。Challenge 起点由
 * Admission 锁存，也是本地 Binding Deadline 的唯一合法起点。 */
typedef struct ucn_i_identity_binding_issue {
    uint64_t transaction_id;
    uint64_t challenge_started_local_us;
    uint64_t challenge_deadline_us;
    uint64_t lease_duration_us;
    uint64_t authority_lease_sequence;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint32_t address;
    uint32_t binding_generation;
    uint32_t authority_generation;
    uint32_t link_generation;
    uint32_t admission_generation;
    uint16_t admission_owner_instance;
    uint8_t address_width;
    uint8_t mode;
    uint8_t principal[UCN_I_IDENTITY_PRINCIPAL_BYTES];
    uint8_t authority_principal[UCN_I_IDENTITY_PRINCIPAL_BYTES];
    uint8_t lease_id[UCN_I_IDENTITY_LEASE_ID_BYTES];
    uint8_t transcript_digest[32];
} ucn_i_identity_binding_issue_t;

/* EN: Persistence baseline already validated by the Coordinator against the
 * current Domain view/body. Prior binding fields make checked-next allocation
 * explicit and auditable without importing the Persistence Owner.
 * 中文：该基线已由 Coordinator 对当前 Domain View/正文完成校验。prior 字段
 * 明确表达 Binding checked-next 关系，无需导入 Persistence Owner。 */
typedef struct ucn_i_identity_durability_base {
    uint64_t domain_id;
    uint64_t prior_foundation_transaction_id;
    uint64_t next_transaction_id;
    uint64_t expected_record_generation;
    uint64_t absolute_deadline_us;
    uint64_t prior_authority_lease_sequence;
    uint32_t prior_address;
    uint32_t prior_binding_generation;
    uint16_t domain_generation;
    uint8_t prior_address_width;
    uint8_t reserved_zero;
    uint8_t prior_principal[UCN_I_IDENTITY_PRINCIPAL_BYTES];
    uint8_t prior_lease_id[UCN_I_IDENTITY_LEASE_ID_BYTES];
    uint8_t expected_body_digest[16];
    ucn_handle_t volatile_continuation;
} ucn_i_identity_durability_base_t;

typedef struct ucn_i_identity_requirement_view {
    const uint8_t *canonical_body;
    uint64_t domain_id;
    uint64_t foundation_transaction_id;
    uint64_t expected_record_generation;
    uint64_t absolute_deadline_us;
    uint64_t transition_fingerprint;
    uint32_t runtime_instance;
    uint32_t body_bytes;
    ucn_handle_t volatile_continuation;
    uint16_t caller_owner_instance;
    uint16_t domain_generation;
    uint16_t schema_id;
    uint16_t schema_version;
    uint16_t operation_kind;
    uint8_t expected_body_digest[16];
} ucn_i_identity_requirement_view_t;

typedef struct ucn_i_identity_durability_proof {
    ucn_handle_t persistence_handle;
    uint64_t domain_id;
    uint64_t record_generation;
    uint64_t foundation_transaction_id;
    uint64_t witness_generation;
    uint64_t transition_fingerprint;
    uint32_t runtime_instance;
    uint32_t body_bytes;
    ucn_handle_t volatile_continuation;
    uint16_t persistence_owner_instance;
    uint16_t caller_owner_instance;
    uint16_t domain_generation;
    uint16_t schema_id;
    uint16_t schema_version;
    uint16_t operation_kind;
    uint16_t reserved_zero;
    uint8_t body_digest[16];
} ucn_i_identity_durability_proof_t;

/* EN: Coordinator may roll a prepared binding back only after Persistence
 * reports an exact terminal failure for the same immutable request. A bare
 * Handle is not failure evidence.
 * 中文：只有 Persistence 对同一不可变请求给出精确终态失败后，Coordinator
 * 才可回滚已准备的 Binding；裸 Handle 不能充当失败证明。 */
typedef struct ucn_i_identity_persistence_failure {
    ucn_handle_t persistence_handle;
    ucn_handle_t volatile_continuation;
    uint64_t domain_id;
    uint64_t foundation_transaction_id;
    uint64_t transition_fingerprint;
    uint32_t runtime_instance;
    ucn_result_t terminal_result;
    uint16_t persistence_owner_instance;
    uint16_t caller_owner_instance;
    uint16_t domain_generation;
    uint16_t schema_id;
    uint16_t schema_version;
    uint16_t operation_kind;
    uint8_t request_state;
    uint8_t reserved_zero[3];
} ucn_i_identity_persistence_failure_t;

typedef struct ucn_i_identity_handle {
    uint32_t runtime_instance;
    uint32_t slot_generation;
    uint32_t binding_generation;
    uint16_t owner_instance;
    uint16_t slot;
} ucn_i_identity_handle_t;

typedef struct ucn_i_identity_binding_view {
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint32_t address;
    uint32_t binding_generation;
    uint32_t authority_generation;
    uint32_t link_generation;
    uint64_t local_deadline_us;
    uint64_t record_generation;
    uint64_t foundation_transaction_id;
    uint64_t witness_generation;
    uint32_t slot_generation;
    uint16_t identity_owner_instance;
    uint16_t persistence_owner_instance;
    uint16_t schema_id;
    uint16_t schema_version;
    uint8_t principal[UCN_I_IDENTITY_PRINCIPAL_BYTES];
    uint8_t body_digest[16];
} ucn_i_identity_binding_view_t;

typedef struct ucn_i_identity_domain_rule {
    uint64_t domain_id;
} ucn_i_identity_domain_rule_t;

typedef struct ucn_i_identity_config {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint16_t owner_instance;
    uint16_t authority_owner_instance;
    uint16_t persistence_business_owner_instance;
    uint16_t persistence_owner_instance;
    uint8_t address_width;
    uint8_t reserved_zero[3];
    const ucn_i_identity_domain_rule_t *domain_rules;
    uint16_t domain_rule_count;
    uint16_t reserved_zero2;
    ucn_i_lock_ops_t state_lock;
} ucn_i_identity_config_t;

typedef struct ucn_i_identity_slot {
    ucn_i_identity_binding_issue_t issue;
    ucn_i_identity_durability_base_t durability;
    ucn_i_identity_binding_view_t active_view;
    ucn_i_identity_binding_view_t previous_view;
    ucn_handle_t persistence_handle;
    uint64_t transition_fingerprint;
    uint8_t body[UCN_I_IDENTITY_BINDING_RECORD_BYTES];
    uint8_t expected_published_digest[16];
    uint32_t slot_generation;
    uint32_t generation_high_water;
    uint8_t occupied;
    uint8_t phase;
    uint8_t persistence_bound;
    uint8_t has_previous;
} ucn_i_identity_slot_t;

typedef struct ucn_i_identity_owner {
    uint32_t magic;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint16_t schema;
    uint16_t owner_instance;
    uint16_t authority_owner_instance;
    uint16_t persistence_business_owner_instance;
    uint16_t persistence_owner_instance;
    uint16_t domain_rule_count;
    uint16_t maintenance_cursor;
    uint8_t address_width;
    uint8_t reserved_zero[3];
    const ucn_i_identity_domain_rule_t *domain_rules;
    ucn_i_lock_ops_t state_lock;
    ucn_i_identity_slot_t bindings[UCN_BINDING_COUNT];
} ucn_i_identity_owner_t;

ucn_result_t ucn_i_identity_binding_record_encode(
    const ucn_i_identity_binding_issue_t *issue,
    uint8_t output[UCN_I_IDENTITY_BINDING_RECORD_BYTES]);
ucn_result_t ucn_i_identity_binding_record_decode(
    const uint8_t input[UCN_I_IDENTITY_BINDING_RECORD_BYTES],
    ucn_i_identity_binding_issue_t *issue_out);
ucn_result_t ucn_i_identity_owner_init(
    ucn_i_identity_owner_t *owner,
    const ucn_i_identity_config_t *config);
ucn_result_t ucn_i_identity_prepare_binding(
    ucn_i_identity_owner_t *owner,
    const ucn_i_identity_binding_issue_t *issue,
    const ucn_i_identity_authority_view_t *authority,
    const ucn_i_identity_durability_base_t *durability,
    uint64_t now_us,
    ucn_i_identity_handle_t *handle_out);
ucn_result_t ucn_i_identity_requirement_get(
    ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle,
    ucn_i_identity_requirement_view_t *requirement_out);
ucn_result_t ucn_i_identity_bind_persistence(
    ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle,
    ucn_handle_t persistence_handle,
    const uint8_t expected_published_digest[16]);
ucn_result_t ucn_i_identity_activate_binding(
    ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle,
    const ucn_i_identity_durability_proof_t *proof,
    const ucn_i_identity_authority_view_t *authority,
    uint64_t now_us,
    ucn_i_identity_binding_view_t *view_out);
ucn_result_t ucn_i_identity_abort_unsubmitted(
    ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle);
ucn_result_t ucn_i_identity_fail_persistence(
    ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle,
    const ucn_i_identity_persistence_failure_t *failure);
ucn_result_t ucn_i_identity_binding_get(
    ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle,
    uint64_t now_us,
    ucn_i_identity_binding_view_t *view_out);
ucn_result_t ucn_i_identity_expire(
    ucn_i_identity_owner_t *owner,
    uint64_t now_us,
    uint16_t budget,
    uint16_t *inspected_out,
    uint16_t *fenced_out);
ucn_result_t ucn_i_identity_retire_fenced(
    ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle);
ucn_result_t ucn_i_identity_owner_destroy(
    ucn_i_identity_owner_t *owner);

#endif
