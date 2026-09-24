#ifndef UCN_INTERNAL_GROUP_H
#define UCN_INTERNAL_GROUP_H

#include "internal/ucn_digest.h"
#include "internal/ucn_owner.h"
#include "ucn/ucn_config.h"

#define UCN_I_GROUP_MAGIC UINT32_C(0x47525036)
#define UCN_I_GROUP_SCHEMA UINT16_C(1)
#define UCN_I_GROUP_RECORD_SCHEMA UINT16_C(1)
#define UCN_I_GROUP_RECORD_SCHEMA_ID UINT16_C(0x0802)
#define UCN_I_GROUP_PERSIST_KIND UINT16_C(0x0802)
#define UCN_I_GROUP_PRINCIPAL_BYTES 16U
#define UCN_I_GROUP_DIGEST_BYTES 16U
#define UCN_I_GROUP_RECORD_HEADER_BYTES 112U
#define UCN_I_GROUP_RECORD_MEMBER_BYTES 28U
#define UCN_I_GROUP_STEP_BUDGET_DEFAULT 4U

#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_I_GROUP_STATIC_COUNT 1U
#define UCN_I_GROUP_DYNAMIC_COUNT 1U
#define UCN_I_GROUP_MEMBER_COUNT 4U
#define UCN_I_GROUP_SEND_COUNT 2U
#define UCN_I_GROUP_ATTEMPT_COUNT 8U
#define UCN_I_GROUP_RX_COUNT 4U
#define UCN_I_GROUP_PERSIST_BODY_LIMIT 256U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_I_GROUP_STATIC_COUNT 4U
#define UCN_I_GROUP_DYNAMIC_COUNT 2U
#define UCN_I_GROUP_MEMBER_COUNT 8U
#define UCN_I_GROUP_SEND_COUNT 8U
#define UCN_I_GROUP_ATTEMPT_COUNT 32U
#define UCN_I_GROUP_RX_COUNT 16U
#define UCN_I_GROUP_PERSIST_BODY_LIMIT 512U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define UCN_I_GROUP_STATIC_COUNT 8U
#define UCN_I_GROUP_DYNAMIC_COUNT 4U
#define UCN_I_GROUP_MEMBER_COUNT 16U
#define UCN_I_GROUP_SEND_COUNT 16U
#define UCN_I_GROUP_ATTEMPT_COUNT 128U
#define UCN_I_GROUP_RX_COUNT 64U
#define UCN_I_GROUP_PERSIST_BODY_LIMIT 1024U
#else
#error "UCN_PROFILE must select NANO, LITE, or FULL"
#endif

#define UCN_I_GROUP_CONTEXT_COUNT \
    (UCN_I_GROUP_STATIC_COUNT + UCN_I_GROUP_DYNAMIC_COUNT)
#define UCN_I_GROUP_RECORD_BYTES                                      \
    (UCN_I_GROUP_RECORD_HEADER_BYTES +                                \
     UCN_I_GROUP_MEMBER_COUNT * UCN_I_GROUP_RECORD_MEMBER_BYTES)

UCN_STATIC_ASSERT(UCN_I_GROUP_MEMBER_COUNT <= 32U,
                  group_member_bitmap_must_fit_u32);
UCN_STATIC_ASSERT(UCN_I_GROUP_RECORD_BYTES <= UCN_I_GROUP_PERSIST_BODY_LIMIT,
                  group_record_must_fit_profile_persistence_body);

typedef uint8_t ucn_i_group_mode_t;
enum {
    UCN_I_GROUP_STATIC = 1,
    UCN_I_GROUP_DYNAMIC = 2
};

typedef uint8_t ucn_i_group_phase_t;
enum {
    UCN_I_GROUP_PENDING = 1,
    UCN_I_GROUP_ACTIVE = 2,
    UCN_I_GROUP_FENCED = 3,
    UCN_I_GROUP_RETIRED = 4,
    UCN_I_GROUP_FAULT = 5
};

typedef uint8_t ucn_i_group_scope_t;
enum {
    UCN_I_GROUP_SCOPE_LOCAL_ONLY = 0,
    UCN_I_GROUP_SCOPE_ANY_MEMBER = 1,
    UCN_I_GROUP_SCOPE_ALL_MEMBERS = 2,
    UCN_I_GROUP_SCOPE_QUORUM = 3,
    UCN_I_GROUP_SCOPE_SUBSET = 4,
    UCN_I_GROUP_SCOPE_COUNT = 5
};

#define UCN_I_GROUP_SCOPE_BIT(scope_) ((uint8_t)(1U << (scope_)))

typedef uint8_t ucn_i_group_plan_t;
enum {
    UCN_I_GROUP_PLAN_UNICAST = 1,
    UCN_I_GROUP_PLAN_TREE = 2
};

typedef uint8_t ucn_i_group_send_phase_t;
enum {
    UCN_I_GROUP_SEND_ACTIVE = 1,
    UCN_I_GROUP_SEND_COMPLETE = 2,
    UCN_I_GROUP_SEND_FAILED = 3
};

typedef uint8_t ucn_i_group_attempt_phase_t;
enum {
    UCN_I_GROUP_ATTEMPT_HELD = 1,
    UCN_I_GROUP_ATTEMPT_READY = 2,
    UCN_I_GROUP_ATTEMPT_SUBMITTED = 3,
    UCN_I_GROUP_ATTEMPT_SUCCEEDED = 4,
    UCN_I_GROUP_ATTEMPT_FAILED = 5,
    UCN_I_GROUP_ATTEMPT_CANCELLED = 6
};

typedef uint8_t ucn_i_group_rx_action_t;
enum {
    UCN_I_GROUP_RX_LOCAL = 1,
    UCN_I_GROUP_RX_RELAY = 2,
    UCN_I_GROUP_RX_LOCAL_AND_RELAY = 3,
    UCN_I_GROUP_RX_REPLAY_RECEIPT = 4
};

typedef struct ucn_i_group_member {
    uint32_t address;
    uint32_t binding_generation;
    uint8_t principal[UCN_I_GROUP_PRINCIPAL_BYTES];
    uint16_t sender_slot;
    uint8_t weight;
    uint8_t reserved_zero;
} ucn_i_group_member_t;

typedef struct ucn_i_group_security_ref {
    uint32_t runtime_instance;
    uint32_t key_generation;
    uint16_t owner_instance;
    uint16_t slot;
    uint16_t generation;
    uint8_t valid;
    uint8_t exact_principal;
    uint8_t context_digest[UCN_I_GROUP_DIGEST_BYTES];
} ucn_i_group_security_ref_t;

typedef struct ucn_i_group_tree_ref {
    uint64_t route_causal_id;
    uint32_t route_generation;
    uint32_t tree_generation;
    uint16_t owner_instance;
    uint16_t slot;
    uint16_t generation;
    uint8_t valid;
    uint8_t reserved_zero;
} ucn_i_group_tree_ref_t;

typedef struct ucn_i_group_authority_facts {
    uint64_t lease_deadline_us;
    uint32_t realm_id;
    uint32_t authority_generation;
    uint16_t owner_instance;
    uint8_t authenticated;
    uint8_t quorum_met;
    uint8_t current;
    uint8_t fenced;
    uint8_t reserved_zero[2];
    uint8_t proof_digest[UCN_I_GROUP_DIGEST_BYTES];
} ucn_i_group_authority_facts_t;

typedef struct ucn_i_group_context_config {
    uint32_t realm_id;
    uint32_t group_id;
    uint32_t group_generation;
    uint32_t policy_generation;
    uint32_t member_generation;
    uint16_t endpoint;
    uint16_t opcode;
    uint8_t mode;
    uint8_t member_count;
    uint8_t unicast_fanout_limit;
    uint8_t quorum_weight;
    uint8_t allowed_scope_mask;
    uint8_t secure_required;
    uint8_t public_static;
    uint8_t reserved_zero;
    ucn_i_group_security_ref_t security;
    ucn_i_group_tree_ref_t tree;
    ucn_i_group_member_t members[UCN_I_GROUP_MEMBER_COUNT];
} ucn_i_group_context_config_t;

typedef struct ucn_i_group_config {
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint16_t owner_instance;
    uint16_t reserved_zero;
    ucn_i_lock_ops_t state_lock;
} ucn_i_group_config_t;

typedef struct ucn_i_group_durability {
    uint64_t domain_id;
    uint64_t foundation_transaction_id;
    uint64_t expected_record_generation;
    uint64_t absolute_deadline_us;
    ucn_handle_t volatile_continuation;
    uint16_t persistence_domain_generation;
    uint16_t schema_id;
    uint16_t schema_version;
} ucn_i_group_durability_t;

typedef struct ucn_i_group_requirement {
    ucn_i_group_durability_t durability;
    uint8_t body[UCN_I_GROUP_RECORD_BYTES];
    uint8_t canonical_body_digest[UCN_I_GROUP_DIGEST_BYTES];
    uint8_t expected_body_digest[UCN_I_GROUP_DIGEST_BYTES];
    uint32_t runtime_instance;
    uint32_t body_bytes;
    uint16_t caller_owner_instance;
    uint16_t operation_kind;
} ucn_i_group_requirement_t;

typedef struct ucn_i_group_proof {
    ucn_handle_t persistence_handle;
    uint64_t domain_id;
    uint64_t foundation_transaction_id;
    uint64_t record_generation;
    uint64_t witness_generation;
    uint32_t runtime_instance;
    uint32_t body_bytes;
    uint16_t persistence_domain_generation;
    uint16_t persistence_owner_instance;
    uint16_t caller_owner_instance;
    uint16_t schema_id;
    uint16_t schema_version;
    uint16_t operation_kind;
    uint16_t reserved_zero;
    uint8_t body_digest[UCN_I_GROUP_DIGEST_BYTES];
} ucn_i_group_proof_t;

typedef struct ucn_i_group_context_slot {
    ucn_i_group_context_config_t current;
    ucn_i_group_context_config_t pending;
    ucn_i_group_authority_facts_t pending_authority;
    ucn_i_group_durability_t pending_durability;
    ucn_handle_t persistence_handle;
    uint32_t pending_high_water;
    uint16_t handle_generation;
    uint8_t phase;
    uint8_t pending_valid;
    uint8_t occupied;
    uint8_t persistence_bound;
    uint8_t pending_retire;
    uint8_t reserved_zero[3];
    uint8_t current_body_digest[UCN_I_GROUP_DIGEST_BYTES];
    uint8_t pending_body_digest[UCN_I_GROUP_DIGEST_BYTES];
} ucn_i_group_context_slot_t;

typedef struct ucn_i_group_send_request {
    uint64_t send_id;
    uint64_t absolute_deadline_us;
    uint32_t expected_group_generation;
    uint32_t expected_policy_generation;
    uint32_t expected_member_generation;
    uint32_t expected_key_generation;
    uint32_t expected_tree_generation;
    uint32_t subset_bitmap;
    uint8_t success_scope;
    uint8_t reliable_receipts;
    uint8_t authenticated_delivery;
    uint8_t exact_principal_delivery;
    uint8_t payload_digest[UCN_I_GROUP_DIGEST_BYTES];
} ucn_i_group_send_request_t;

typedef struct ucn_i_group_send_slot {
    ucn_i_group_member_t members[UCN_I_GROUP_MEMBER_COUNT];
    ucn_i_group_security_ref_t security;
    ucn_i_group_tree_ref_t tree;
    ucn_result_t member_results[UCN_I_GROUP_MEMBER_COUNT];
    uint8_t payload_digest[UCN_I_GROUP_DIGEST_BYTES];
    uint64_t send_id;
    uint64_t deadline_us;
    uint32_t group_id;
    uint32_t group_generation;
    uint32_t policy_generation;
    uint32_t member_generation;
    uint32_t requested_bitmap;
    uint32_t result_bitmap;
    uint32_t success_bitmap;
    uint16_t endpoint;
    uint16_t opcode;
    uint16_t handle_generation;
    uint8_t member_count;
    uint8_t quorum_weight;
    uint8_t success_scope;
    uint8_t plan;
    uint8_t phase;
    uint8_t completion_satisfied;
    uint8_t occupied;
    uint8_t reserved_zero;
} ucn_i_group_send_slot_t;

typedef struct ucn_i_group_attempt_slot {
    ucn_i_group_member_t target;
    uint64_t send_id;
    uint32_t group_id;
    uint16_t handle_generation;
    uint16_t parent_slot;
    uint16_t member_index;
    uint8_t parent_is_rx;
    uint8_t plan;
    uint8_t phase;
    uint8_t occupied;
    uint8_t reserved_zero[3];
} ucn_i_group_attempt_slot_t;

typedef struct ucn_i_group_attempt_view {
    ucn_handle_t attempt;
    uint64_t send_id;
    uint32_t group_id;
    uint32_t group_generation;
    uint32_t policy_generation;
    uint32_t member_generation;
    uint32_t member_address;
    uint32_t member_binding_generation;
    ucn_i_group_security_ref_t security;
    ucn_i_group_tree_ref_t tree;
    uint8_t payload_digest[UCN_I_GROUP_DIGEST_BYTES];
    uint16_t endpoint;
    uint16_t opcode;
    uint16_t sender_slot;
    uint8_t plan;
    uint8_t reserved_zero;
} ucn_i_group_attempt_view_t;

typedef struct ucn_i_group_member_receipt {
    uint64_t send_id;
    uint32_t group_id;
    uint32_t group_generation;
    uint32_t member_address;
    uint32_t member_binding_generation;
    uint8_t member_principal[UCN_I_GROUP_PRINCIPAL_BYTES];
    uint8_t payload_digest[UCN_I_GROUP_DIGEST_BYTES];
    ucn_result_t terminal_result;
    uint8_t authenticated;
    uint8_t terminal;
    uint8_t reserved_zero[2];
} ucn_i_group_member_receipt_t;

typedef struct ucn_i_group_send_view {
    uint64_t send_id;
    uint32_t group_id;
    uint32_t requested_bitmap;
    uint32_t result_bitmap;
    uint32_t success_bitmap;
    uint8_t success_scope;
    uint8_t plan;
    uint8_t phase;
    uint8_t completion_satisfied;
} ucn_i_group_send_view_t;

typedef struct ucn_i_group_receive_facts {
    uint64_t send_id;
    uint64_t absolute_deadline_us;
    uint32_t group_id;
    uint32_t group_generation;
    uint32_t policy_generation;
    uint32_t member_generation;
    uint32_t key_generation;
    uint32_t source_address;
    uint32_t source_binding_generation;
    uint32_t relay_member_bitmap;
    uint16_t endpoint;
    uint16_t opcode;
    uint16_t sender_slot;
    uint8_t source_principal[UCN_I_GROUP_PRINCIPAL_BYTES];
    uint8_t payload_digest[UCN_I_GROUP_DIGEST_BYTES];
    uint8_t fresh_authenticated;
    uint8_t authenticated_replay_candidate;
    uint8_t public_unsecured;
    uint8_t acl_authorized;
    uint8_t exact_principal;
    uint8_t local_member;
    uint8_t relay_egress_count;
    uint8_t reserved_zero;
} ucn_i_group_receive_facts_t;

typedef struct ucn_i_group_security_commit {
    uint64_t send_id;
    uint32_t group_id;
    uint32_t group_generation;
    uint32_t key_generation;
    uint16_t security_owner_instance;
    uint8_t replay_committed;
    uint8_t reserved_zero;
    uint8_t payload_digest[UCN_I_GROUP_DIGEST_BYTES];
} ucn_i_group_security_commit_t;

typedef struct ucn_i_group_rx_slot {
    ucn_i_group_receive_facts_t facts;
    ucn_i_group_security_ref_t security;
    ucn_i_group_tree_ref_t tree;
    ucn_result_t application_result;
    uint16_t handle_generation;
    uint8_t action;
    uint8_t phase;
    uint8_t occupied;
    uint8_t published;
    uint8_t application_complete;
    uint8_t reserved_zero;
} ucn_i_group_rx_slot_t;

typedef struct ucn_i_group_rx_view {
    ucn_handle_t receipt;
    uint64_t send_id;
    uint32_t group_id;
    ucn_result_t application_result;
    uint8_t action;
    uint8_t application_complete;
    uint8_t stable_receipt;
    uint8_t reserved_zero;
} ucn_i_group_rx_view_t;

typedef struct ucn_i_group_dependency_event {
    uint32_t group_id;
    uint32_t group_generation;
    uint32_t policy_generation;
    uint32_t member_generation;
    uint32_t key_generation;
    uint32_t tree_generation;
    uint8_t security_current;
    uint8_t tree_current;
    uint8_t authority_current;
    uint8_t reserved_zero;
} ucn_i_group_dependency_event_t;

typedef struct ucn_i_group_owner {
    uint32_t magic;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint32_t dynamic_id_high_water;
    uint16_t schema;
    uint16_t owner_instance;
    ucn_i_lock_ops_t state_lock;
    ucn_i_group_context_slot_t contexts[UCN_I_GROUP_CONTEXT_COUNT];
    ucn_i_group_send_slot_t sends[UCN_I_GROUP_SEND_COUNT];
    ucn_i_group_attempt_slot_t attempts[UCN_I_GROUP_ATTEMPT_COUNT];
    ucn_i_group_rx_slot_t receipts[UCN_I_GROUP_RX_COUNT];
    ucn_i_sha256_workspace_t digest_workspace;
    ucn_i_group_requirement_t requirement_staging;
    ucn_i_group_context_config_t config_staging;
    uint16_t attempt_cursor;
    uint16_t step_cursor;
} ucn_i_group_owner_t;

ucn_result_t ucn_i_group_owner_init(ucn_i_group_owner_t *owner,
                                    const ucn_i_group_config_t *config);
ucn_result_t ucn_i_group_owner_destroy(ucn_i_group_owner_t *owner);
ucn_result_t ucn_i_group_install_static(
    ucn_i_group_owner_t *owner, uint16_t static_slot,
    const ucn_i_group_context_config_t *config,
    bool manifest_authenticated, bool manifest_anti_rollback,
    ucn_handle_t *group_out);
ucn_result_t ucn_i_group_open(ucn_i_group_owner_t *owner,
                              uint32_t group_id,
                              uint32_t group_generation,
                              uint32_t policy_generation,
                              ucn_handle_t *group_out);
ucn_result_t ucn_i_group_context_view(
    ucn_i_group_owner_t *owner, ucn_handle_t group,
    ucn_i_group_context_config_t *config_out, uint8_t *phase_out);
ucn_result_t ucn_i_group_record_encode(
    const ucn_i_group_context_config_t *config, uint8_t record_phase,
    uint32_t dynamic_id_high_water,
    uint8_t body_out[UCN_I_GROUP_RECORD_BYTES]);
ucn_result_t ucn_i_group_admin_prepare(
    ucn_i_group_owner_t *owner,
    const ucn_i_group_context_config_t *next_config,
    const ucn_i_group_authority_facts_t *authority,
    const ucn_i_group_durability_t *durability, uint64_t now_us,
    ucn_handle_t *group_out,
    ucn_i_group_requirement_t *requirement_out);
ucn_result_t ucn_i_group_admin_retire_prepare(
    ucn_i_group_owner_t *owner, ucn_handle_t group,
    const ucn_i_group_authority_facts_t *authority,
    const ucn_i_group_durability_t *durability, uint64_t now_us,
    ucn_i_group_requirement_t *requirement_out);
ucn_result_t ucn_i_group_bind_persistence(
    ucn_i_group_owner_t *owner, ucn_handle_t group,
    ucn_handle_t persistence_handle,
    const uint8_t published_body_digest[UCN_I_GROUP_DIGEST_BYTES]);
ucn_result_t ucn_i_group_accept_proof(
    ucn_i_group_owner_t *owner, ucn_handle_t group,
    const ucn_i_group_proof_t *proof,
    const ucn_i_group_authority_facts_t *authority, uint64_t now_us);
ucn_result_t ucn_i_group_import(
    ucn_i_group_owner_t *owner, const uint8_t *body, size_t body_bytes,
    const ucn_i_group_durability_t *durability,
    const uint8_t published_body_digest[UCN_I_GROUP_DIGEST_BYTES],
    ucn_handle_t *group_out);

ucn_result_t ucn_i_group_send_begin(
    ucn_i_group_owner_t *owner, ucn_handle_t group,
    const ucn_i_group_send_request_t *request,
    uint64_t now_us,
    ucn_handle_t *send_out);
ucn_result_t ucn_i_group_attempt_peek(
    ucn_i_group_owner_t *owner,
    ucn_i_group_attempt_view_t *attempt_out);
ucn_result_t ucn_i_group_attempt_note_submitted(
    ucn_i_group_owner_t *owner, ucn_handle_t attempt);
ucn_result_t ucn_i_group_attempt_complete(
    ucn_i_group_owner_t *owner, ucn_handle_t attempt,
    ucn_result_t terminal_result);
ucn_result_t ucn_i_group_send_accept_receipt(
    ucn_i_group_owner_t *owner, ucn_handle_t send,
    const ucn_i_group_member_receipt_t *receipt);
ucn_result_t ucn_i_group_send_view(ucn_i_group_owner_t *owner,
                                   ucn_handle_t send,
                                   ucn_i_group_send_view_t *view_out);
ucn_result_t ucn_i_group_send_retire(ucn_i_group_owner_t *owner,
                                     ucn_handle_t send);

ucn_result_t ucn_i_group_receive_preflight(
    ucn_i_group_owner_t *owner,
    const ucn_i_group_receive_facts_t *facts, uint64_t now_us,
    ucn_handle_t *reservation_out,
    ucn_i_group_rx_view_t *replay_out);
ucn_result_t ucn_i_group_receive_publish(
    ucn_i_group_owner_t *owner, ucn_handle_t reservation,
    const ucn_i_group_security_commit_t *commit);
ucn_result_t ucn_i_group_receive_abort(
    ucn_i_group_owner_t *owner, ucn_handle_t reservation);
ucn_result_t ucn_i_group_receive_complete(
    ucn_i_group_owner_t *owner, ucn_handle_t receipt,
    ucn_result_t application_result);
ucn_result_t ucn_i_group_receive_view(
    ucn_i_group_owner_t *owner, ucn_handle_t receipt,
    ucn_i_group_rx_view_t *view_out);
ucn_result_t ucn_i_group_receive_retire(
    ucn_i_group_owner_t *owner, ucn_handle_t receipt);

ucn_result_t ucn_i_group_dependency_change(
    ucn_i_group_owner_t *owner,
    const ucn_i_group_dependency_event_t *event);
ucn_result_t ucn_i_group_step(ucn_i_group_owner_t *owner,
                              uint64_t now_us, uint16_t budget,
                              uint16_t *inspected_out,
                              uint16_t *changed_out);

/* Cross-file helpers. Caller holds owner->state_lock. */
bool ucn_i_group_p_owner_valid(const ucn_i_group_owner_t *owner);
bool ucn_i_group_p_context_valid(
    const ucn_i_group_context_config_t *config, bool pending_allowed);
bool ucn_i_group_p_handle_matches(const ucn_i_group_owner_t *owner,
                                  ucn_handle_t handle,
                                  uint16_t *slot_out);
bool ucn_i_group_p_attempt_handle_matches(
    const ucn_i_group_owner_t *owner, ucn_handle_t handle,
    uint16_t *slot_out);
bool ucn_i_group_p_rx_handle_matches(const ucn_i_group_owner_t *owner,
                                     ucn_handle_t handle,
                                     uint16_t *slot_out);
void ucn_i_group_p_make_handle(const ucn_i_group_owner_t *owner,
                               uint16_t slot, uint16_t generation,
                               uint8_t kind, ucn_handle_t *handle_out);
void ucn_i_group_p_release_rx_attempts(ucn_i_group_owner_t *owner,
                                       uint16_t rx_slot,
                                       bool only_held);
void ucn_i_group_p_recompute_send(ucn_i_group_send_slot_t *send);

#endif
