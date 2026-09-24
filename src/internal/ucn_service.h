#ifndef UCN_SERVICE_INTERNAL_H
#define UCN_SERVICE_INTERNAL_H

#include "internal/ucn_owner.h"
#include "ucn/ucn_config.h"
#include "ucn/ucn_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UCN_I_SERVICE_SCHEMA UINT16_C(1)
#define UCN_I_SERVICE_PRINCIPAL_BYTES 16U
#define UCN_I_SERVICE_DIGEST_BYTES 16U
#define UCN_I_SERVICE_CLASS_COUNT 4U
#define UCN_I_SERVICE_LATEST_KEY_BYTES 16U
#define UCN_I_SERVICE_OPERATION_SCHEMA UINT16_C(1)
#define UCN_I_SERVICE_OPERATION_PERSIST_KIND UINT16_C(0x0701)
#define UCN_I_SERVICE_OPERATION_ID_PERSIST_KIND UINT16_C(0x0702)

#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_I_SERVICE_REQUEST_COUNT 2U
#define UCN_I_SERVICE_RECEIPT_COUNT 2U
#define UCN_I_SERVICE_QOS_COUNT 8U
#define UCN_I_SERVICE_LATEST_COUNT 2U
#define UCN_I_SERVICE_SOURCE_QUOTA 2U
#define UCN_I_SERVICE_FLOW_QUOTA 1U
#define UCN_I_SERVICE_RESULT_BYTES 32U
#define UCN_I_SERVICE_OPERATION_COUNT 1U
#define UCN_I_SERVICE_OPERATION_ID_INTERVAL_SIZE 16U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_I_SERVICE_REQUEST_COUNT 8U
#define UCN_I_SERVICE_RECEIPT_COUNT 8U
#define UCN_I_SERVICE_QOS_COUNT 32U
#define UCN_I_SERVICE_LATEST_COUNT 8U
#define UCN_I_SERVICE_SOURCE_QUOTA 4U
#define UCN_I_SERVICE_FLOW_QUOTA 2U
#define UCN_I_SERVICE_RESULT_BYTES 64U
#define UCN_I_SERVICE_OPERATION_COUNT 4U
#define UCN_I_SERVICE_OPERATION_ID_INTERVAL_SIZE 64U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define UCN_I_SERVICE_REQUEST_COUNT 16U
#define UCN_I_SERVICE_RECEIPT_COUNT 24U
#define UCN_I_SERVICE_QOS_COUNT 96U
#define UCN_I_SERVICE_LATEST_COUNT 24U
#define UCN_I_SERVICE_SOURCE_QUOTA 8U
#define UCN_I_SERVICE_FLOW_QUOTA 4U
#define UCN_I_SERVICE_RESULT_BYTES 128U
#define UCN_I_SERVICE_OPERATION_COUNT 8U
#define UCN_I_SERVICE_OPERATION_ID_INTERVAL_SIZE 256U
#else
#error "UCN_PROFILE must be UCN_PROFILE_NANO, UCN_PROFILE_LITE, or UCN_PROFILE_FULL"
#endif

UCN_STATIC_ASSERT(UCN_I_SERVICE_QOS_COUNT <= UINT16_MAX,
                  service_qos_index_must_fit_u16);

#define UCN_I_SERVICE_OPERATION_BODY_BYTES (110U + UCN_I_SERVICE_RESULT_BYTES)
#define UCN_I_SERVICE_OPERATION_ID_BODY_BYTES 20U

typedef struct ucn_i_service_binding {
    uint32_t address;
    uint32_t generation;
    uint8_t principal[UCN_I_SERVICE_PRINCIPAL_BYTES];
} ucn_i_service_binding_t;

typedef struct ucn_i_service_security_facts {
    uint32_t session_generation;
    uint32_t key_generation;
    uint32_t policy_generation;
    uint8_t origin_security;
    uint8_t acl_authorized;
    uint8_t authenticated_replay_candidate;
    uint8_t reserved_zero;
} ucn_i_service_security_facts_t;

typedef struct ucn_i_service_key {
    ucn_i_service_binding_t client;
    ucn_i_service_binding_t server;
    ucn_i_service_security_facts_t security;
    uint64_t operation_id;
    uint32_t realm;
    uint16_t service_id;
    uint16_t opcode;
} ucn_i_service_key_t;

typedef uint8_t ucn_i_service_request_phase_t;
enum {
    UCN_I_SERVICE_REQUEST_WAIT_SEND = 1,
    UCN_I_SERVICE_REQUEST_WAIT_RESULT = 2,
    UCN_I_SERVICE_REQUEST_COMPLETE = 3,
    UCN_I_SERVICE_REQUEST_FAILED = 4,
    UCN_I_SERVICE_REQUEST_CANCELLED = 5
};

typedef struct ucn_i_service_result {
    ucn_result_t application_result;
    uint16_t bytes;
    uint8_t payload[UCN_I_SERVICE_RESULT_BYTES];
} ucn_i_service_result_t;

typedef struct ucn_i_service_request_view {
    ucn_i_service_key_t key;
    uint64_t deadline_us;
    ucn_result_t terminal_result;
    uint16_t result_bytes;
    uint8_t phase;
    uint8_t reserved_zero;
} ucn_i_service_request_view_t;

typedef uint8_t ucn_i_service_receive_action_t;
enum {
    UCN_I_SERVICE_RECEIVE_INVOKE = 1,
    UCN_I_SERVICE_RECEIVE_REPLAY_RESULT = 2
};

typedef struct ucn_i_service_qos_item {
    ucn_handle_t sendable;
    uint8_t latest_key[UCN_I_SERVICE_LATEST_KEY_BYTES];
    uint64_t enqueue_order;
    uint64_t deadline_us;
    uint32_t source_quota_key;
    uint32_t flow_quota_key;
    uint8_t traffic_class;
    uint8_t latest;
    uint8_t control_authorized;
    uint8_t reserved_zero;
} ucn_i_service_qos_item_t;

typedef struct ucn_i_service_qos_view {
    ucn_i_service_qos_item_t item;
    uint8_t selected;
    uint8_t submitted;
    uint8_t reserved_zero[6];
} ucn_i_service_qos_view_t;

typedef uint8_t ucn_i_service_operation_phase_t;
enum {
    UCN_I_SERVICE_OPERATION_PREPARED = 1,
    UCN_I_SERVICE_OPERATION_EXECUTING = 2,
    UCN_I_SERVICE_OPERATION_COMMITTED_RESULT = 3,
    UCN_I_SERVICE_OPERATION_ABORTED_NO_EFFECT = 4,
    UCN_I_SERVICE_OPERATION_IN_DOUBT = 5,
    UCN_I_SERVICE_OPERATION_TOMBSTONED = 6,
    UCN_I_SERVICE_OPERATION_PERSIST_PENDING = 7,
    UCN_I_SERVICE_OPERATION_FAULTED = 8
};

typedef struct ucn_i_service_operation_key {
    ucn_i_service_key_t service;
    uint8_t request_digest[UCN_I_SERVICE_DIGEST_BYTES];
} ucn_i_service_operation_key_t;

typedef struct ucn_i_service_operation_durability {
    uint64_t domain_id;
    uint64_t foundation_transaction_id;
    uint64_t expected_record_generation;
    uint64_t absolute_deadline_us;
    ucn_handle_t volatile_continuation;
    uint16_t domain_generation;
    uint16_t schema_id;
    uint16_t schema_version;
} ucn_i_service_operation_durability_t;

typedef struct ucn_i_service_operation_requirement {
    ucn_handle_t continuation;
    ucn_i_service_operation_durability_t durability;
    uint8_t canonical_body_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint8_t expected_body_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint8_t body[UCN_I_SERVICE_OPERATION_BODY_BYTES];
    uint32_t runtime_instance;
    uint32_t body_bytes;
    uint16_t caller_owner_instance;
    uint16_t operation_kind;
    uint8_t next_phase;
    uint8_t reserved_zero[3];
} ucn_i_service_operation_requirement_t;

typedef struct ucn_i_service_operation_proof {
    ucn_handle_t continuation;
    ucn_handle_t persistence_handle;
    uint64_t domain_id;
    uint64_t foundation_transaction_id;
    uint64_t record_generation;
    uint64_t witness_generation;
    uint32_t runtime_instance;
    uint32_t body_bytes;
    uint16_t domain_generation;
    uint16_t persistence_owner_instance;
    uint16_t caller_owner_instance;
    uint16_t schema_id;
    uint16_t schema_version;
    uint16_t operation_kind;
    uint16_t reserved_zero16;
    uint8_t body_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint8_t next_phase;
    uint8_t reserved_zero[3];
} ucn_i_service_operation_proof_t;

typedef struct ucn_i_service_operation_view {
    ucn_i_service_operation_key_t key;
    ucn_i_service_result_t result;
    uint8_t phase;
    uint8_t executor_observed;
    uint8_t reply_ready;
    uint8_t reserved_zero;
} ucn_i_service_operation_view_t;

typedef struct ucn_i_service_operation_id_requirement {
    ucn_i_service_operation_durability_t durability;
    uint8_t canonical_body_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint8_t expected_body_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint8_t body[UCN_I_SERVICE_OPERATION_ID_BODY_BYTES];
    uint64_t proposed_high_water;
    uint32_t runtime_instance;
    uint32_t parent_generation;
    uint16_t caller_owner_instance;
    uint16_t operation_kind;
} ucn_i_service_operation_id_requirement_t;

typedef struct ucn_i_service_operation_id_proof {
    ucn_handle_t persistence_handle;
    uint64_t domain_id;
    uint64_t foundation_transaction_id;
    uint64_t record_generation;
    uint64_t witness_generation;
    uint32_t runtime_instance;
    uint32_t body_bytes;
    uint16_t domain_generation;
    uint16_t persistence_owner_instance;
    uint16_t caller_owner_instance;
    uint16_t schema_id;
    uint16_t schema_version;
    uint16_t operation_kind;
    uint16_t reserved_zero;
    uint8_t body_digest[UCN_I_SERVICE_DIGEST_BYTES];
} ucn_i_service_operation_id_proof_t;

typedef struct ucn_i_service_operation_id_view {
    uint64_t next_id;
    uint64_t reserved_through;
    uint32_t parent_generation;
    uint8_t interval_ready;
    uint8_t persist_pending;
    uint8_t faulted;
    uint8_t reserved_zero;
} ucn_i_service_operation_id_view_t;

typedef struct ucn_i_service_config {
    uint64_t receipt_lifetime_us;
    uint32_t runtime_instance;
    uint16_t owner_instance;
    uint16_t reserved_zero;
    ucn_i_lock_ops_t state_lock;
} ucn_i_service_config_t;

typedef struct ucn_i_service_request_record {
    ucn_i_service_key_t key;
    ucn_i_service_result_t result;
    uint64_t deadline_us;
    ucn_result_t terminal_result;
    uint16_t generation;
    uint8_t occupied;
    uint8_t phase;
} ucn_i_service_request_record_t;

typedef struct ucn_i_service_receipt_record {
    ucn_i_service_key_t key;
    ucn_i_service_result_t result;
    uint8_t request_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint64_t expires_at_us;
    uint16_t generation;
    uint8_t occupied;
    uint8_t terminal;
} ucn_i_service_receipt_record_t;

typedef struct ucn_i_service_qos_record {
    ucn_i_service_qos_item_t item;
    uint16_t generation;
    uint8_t occupied;
    uint8_t selected;
    uint8_t submitted;
} ucn_i_service_qos_record_t;

typedef struct ucn_i_service_operation_record {
    ucn_i_service_operation_key_t key;
    ucn_i_service_operation_durability_t durability;
    ucn_i_service_result_t result;
    ucn_handle_t persistence_handle;
    uint8_t canonical_body_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint8_t current_published_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint8_t pending_published_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint32_t pending_body_bytes;
    uint16_t generation;
    uint8_t occupied;
    uint8_t phase;
    uint8_t stable_phase;
    uint8_t pending_phase;
    uint8_t executor_observed;
    uint8_t reply_ready;
    uint8_t persistence_bound;
    uint8_t reserved_zero;
} ucn_i_service_operation_record_t;

typedef struct ucn_i_service_owner {
    uint32_t magic;
    uint32_t runtime_instance;
    uint64_t receipt_lifetime_us;
    uint64_t next_enqueue_order;
    uint64_t operation_id_next;
    uint64_t operation_id_reserved_through;
    uint64_t operation_id_pending_high_water;
    ucn_i_service_operation_durability_t operation_id_durability;
    ucn_handle_t operation_id_persistence_handle;
    uint8_t operation_id_current_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint8_t operation_id_pending_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint8_t operation_id_canonical_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint32_t operation_id_parent_generation;
    uint16_t schema;
    uint16_t owner_instance;
    uint16_t class_cursor[UCN_I_SERVICE_CLASS_COUNT];
    uint16_t maintain_cursor;
    uint8_t schedule_cursor;
    uint8_t operation_id_ready;
    uint8_t operation_id_persist_pending;
    uint8_t operation_id_persistence_bound;
    uint8_t operation_id_faulted;
    uint8_t reserved_zero[3];
    ucn_i_lock_ops_t state_lock;
    ucn_i_service_request_record_t requests[UCN_I_SERVICE_REQUEST_COUNT];
    ucn_i_service_receipt_record_t receipts[UCN_I_SERVICE_RECEIPT_COUNT];
    ucn_i_service_qos_record_t qos[UCN_I_SERVICE_QOS_COUNT];
    ucn_i_service_operation_record_t operations[UCN_I_SERVICE_OPERATION_COUNT];
} ucn_i_service_owner_t;

ucn_result_t ucn_i_service_owner_init(ucn_i_service_owner_t *owner,
                                      const ucn_i_service_config_t *config);
ucn_result_t ucn_i_service_owner_destroy(ucn_i_service_owner_t *owner);

ucn_result_t ucn_i_service_request_begin(ucn_i_service_owner_t *owner,
                                         const ucn_i_service_key_t *key,
                                         uint64_t deadline_us,
                                         ucn_handle_t *handle_out);
ucn_result_t ucn_i_service_request_note_sent(ucn_i_service_owner_t *owner,
                                             ucn_handle_t handle,
                                             uint64_t now_us);
ucn_result_t ucn_i_service_request_accept_result(
    ucn_i_service_owner_t *owner,
    ucn_handle_t handle,
    const ucn_i_service_key_t *response_key,
    const ucn_i_service_result_t *result,
    uint64_t now_us);
ucn_result_t ucn_i_service_request_view(ucn_i_service_owner_t *owner,
                                        ucn_handle_t handle,
                                        ucn_i_service_request_view_t *view_out);
ucn_result_t ucn_i_service_request_copy_result(
    ucn_i_service_owner_t *owner,
    ucn_handle_t handle,
    ucn_i_service_result_t *result_out);
ucn_result_t ucn_i_service_request_cancel(ucn_i_service_owner_t *owner,
                                          ucn_handle_t handle);
ucn_result_t ucn_i_service_request_retire(ucn_i_service_owner_t *owner,
                                          ucn_handle_t handle);

ucn_result_t ucn_i_service_receive_request(
    ucn_i_service_owner_t *owner,
    const ucn_i_service_key_t *key,
    const uint8_t request_digest[UCN_I_SERVICE_DIGEST_BYTES],
    uint64_t now_us,
    ucn_handle_t *receipt_out,
    ucn_i_service_receive_action_t *action_out);
ucn_result_t ucn_i_service_commit_result(
    ucn_i_service_owner_t *owner,
    ucn_handle_t receipt,
    const ucn_i_service_result_t *result,
    uint64_t now_us);
ucn_result_t ucn_i_service_receipt_copy_result(
    ucn_i_service_owner_t *owner,
    ucn_handle_t receipt,
    ucn_i_service_result_t *result_out);

ucn_result_t ucn_i_service_qos_enqueue(ucn_i_service_owner_t *owner,
                                       const ucn_i_service_qos_item_t *item,
                                       ucn_handle_t *qos_handle_out,
                                       ucn_handle_t *superseded_out);
ucn_result_t ucn_i_service_qos_pick(ucn_i_service_owner_t *owner,
                                    uint64_t now_us,
                                    ucn_handle_t *qos_handle_out,
                                    ucn_handle_t *sendable_out);
ucn_result_t ucn_i_service_qos_note_submitted(ucn_i_service_owner_t *owner,
                                              ucn_handle_t qos_handle);
ucn_result_t ucn_i_service_qos_release_pick(ucn_i_service_owner_t *owner,
                                            ucn_handle_t qos_handle);
ucn_result_t ucn_i_service_qos_remove(ucn_i_service_owner_t *owner,
                                      ucn_handle_t qos_handle);
ucn_result_t ucn_i_service_qos_view(ucn_i_service_owner_t *owner,
                                    ucn_handle_t qos_handle,
                                    ucn_i_service_qos_view_t *view_out);
ucn_result_t ucn_i_service_maintain(ucn_i_service_owner_t *owner,
                                    uint64_t now_us,
                                    uint16_t budget,
                                    uint16_t *inspected_out,
                                    uint16_t *changed_out);

ucn_result_t ucn_i_service_operation_begin(
    ucn_i_service_owner_t *owner,
    const ucn_i_service_operation_key_t *key,
    const ucn_i_service_operation_durability_t *durability,
    ucn_handle_t *operation_out);
ucn_result_t ucn_i_service_operation_import(
    ucn_i_service_owner_t *owner,
    const uint8_t *body,
    uint32_t body_bytes,
    const ucn_i_service_operation_durability_t *loaded_durability,
    const uint8_t loaded_published_digest[UCN_I_SERVICE_DIGEST_BYTES],
    ucn_handle_t *operation_out);
ucn_result_t ucn_i_service_operation_requirement_get(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation,
    ucn_i_service_operation_requirement_t *requirement_out);
ucn_result_t ucn_i_service_operation_bind_persistence(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation,
    ucn_handle_t persistence_handle,
    const uint8_t published_body_digest[UCN_I_SERVICE_DIGEST_BYTES]);
ucn_result_t ucn_i_service_operation_accept_proof(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation,
    const ucn_i_service_operation_proof_t *proof);
ucn_result_t ucn_i_service_operation_prepare_executing(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation,
    const ucn_i_service_operation_durability_t *durability);
ucn_result_t ucn_i_service_operation_mark_executor_observed(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation);
ucn_result_t ucn_i_service_operation_prepare_terminal(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation,
    ucn_i_service_operation_phase_t terminal_phase,
    const ucn_i_service_result_t *result,
    const ucn_i_service_operation_durability_t *durability);
ucn_result_t ucn_i_service_operation_view(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation,
    ucn_i_service_operation_view_t *view_out);
ucn_result_t ucn_i_service_operation_retire(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation,
    bool authenticated_result_ack,
    bool retention_elapsed,
    uint64_t retired_operation_floor);

ucn_result_t ucn_i_service_operation_id_import(
    ucn_i_service_owner_t *owner,
    const uint8_t body[UCN_I_SERVICE_OPERATION_ID_BODY_BYTES],
    const ucn_i_service_operation_durability_t *loaded_durability,
    const uint8_t loaded_published_digest[UCN_I_SERVICE_DIGEST_BYTES]);
ucn_result_t ucn_i_service_operation_id_prepare_interval(
    ucn_i_service_owner_t *owner,
    uint32_t parent_generation,
    const ucn_i_service_operation_durability_t *durability,
    ucn_i_service_operation_id_requirement_t *requirement_out);
ucn_result_t ucn_i_service_operation_id_bind_persistence(
    ucn_i_service_owner_t *owner,
    ucn_handle_t persistence_handle,
    const uint8_t published_body_digest[UCN_I_SERVICE_DIGEST_BYTES]);
ucn_result_t ucn_i_service_operation_id_accept_proof(
    ucn_i_service_owner_t *owner,
    const ucn_i_service_operation_id_proof_t *proof);
ucn_result_t ucn_i_service_operation_id_take(
    ucn_i_service_owner_t *owner,
    uint64_t *operation_id_out);
ucn_result_t ucn_i_service_operation_id_view(
    ucn_i_service_owner_t *owner,
    ucn_i_service_operation_id_view_t *view_out);

#endif
