#ifndef UCN_INTERNAL_PERSISTENCE_H
#define UCN_INTERNAL_PERSISTENCE_H

#include "ucn/ucn_persistence.h"

#define UCN_I_PERSIST_OWNER_MAGIC UINT32_C(0x5543504F)
#define UCN_I_PERSIST_GATE_MAGIC UINT32_C(0x55435047)
#define UCN_I_PERSIST_OWNER_SCHEMA UINT16_C(2)
#define UCN_I_PERSIST_GATE_SCHEMA UINT16_C(1)
#define UCN_I_PERSIST_DIGEST_SUITE_BLAKE2S_128 \
    UCN_PERSIST_DIGEST_BLAKE2S_128
#define UCN_I_PERSIST_INVALID_INDEX UINT8_C(0xFF)

typedef uint8_t ucn_i_persist_owner_phase_t;
enum {
    UCN_I_PERSIST_OWNER_INITIALIZED = 1,
    UCN_I_PERSIST_OWNER_RECOVERING = 2,
    UCN_I_PERSIST_OWNER_READY = 3,
    UCN_I_PERSIST_OWNER_FAULTED = 4
};

/* EN: Persistence transactions and proofs are internal Owner SPI objects.
 * Applications configure the Provider/Manifest through the installed product
 * header, but only the Runtime Coordinator may route these objects.
 * 中文：持久化事务和证明属于内部 Owner SPI 对象。应用可通过安装的产品头
 * 配置 Provider/Manifest，但只有 Runtime Coordinator 可以路由这些对象。 */
typedef struct ucn_persistence_request {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t runtime_instance;
    uint16_t caller_owner_instance;
    uint16_t domain_generation;
    ucn_persist_domain_key_t domain;
    uint64_t foundation_transaction_id;
    uint64_t expected_record_generation;
    uint64_t absolute_deadline_us;
    uint64_t business_transition_digest;
    const uint8_t *canonical_body;
    uint32_t body_bytes;
    uint16_t schema_id;
    uint16_t schema_version;
    uint16_t operation_kind;
    uint16_t reserved_zero;
    uint8_t expected_body_digest[UCN_PERSIST_DIGEST_BYTES];
    ucn_handle_t volatile_continuation;
} ucn_persistence_request_t;

typedef uint8_t ucn_persist_domain_state_t;
enum {
    UCN_PERSIST_DOMAIN_RECOVERING = 1,
    UCN_PERSIST_DOMAIN_READY = 2,
    UCN_PERSIST_DOMAIN_FAULTED = 3
};

typedef struct ucn_persistence_domain_view {
    uint16_t struct_size;
    uint16_t api_version;
    ucn_persist_domain_key_t domain;
    uint64_t record_generation;
    uint32_t body_bytes;
    uint16_t schema_id;
    uint16_t schema_version;
    uint16_t domain_generation;
    uint8_t state;
    uint8_t active_slot;
    uint8_t required;
    uint8_t pending;
    uint8_t body_digest[UCN_PERSIST_DIGEST_BYTES];
} ucn_persistence_domain_view_t;

typedef struct ucn_persistence_proof {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t runtime_instance;
    uint16_t persistence_owner_instance;
    uint16_t caller_owner_instance;
    uint16_t domain_generation;
    uint16_t operation_kind;
    ucn_persist_domain_key_t domain;
    uint64_t record_generation;
    uint64_t foundation_transaction_id;
    uint64_t witness_generation;
    uint32_t body_bytes;
    uint8_t active_slot;
    uint8_t reserved_zero[3];
    uint8_t body_digest[UCN_PERSIST_DIGEST_BYTES];
} ucn_persistence_proof_t;

typedef struct ucn_persistence_step_result {
    uint16_t struct_size;
    uint16_t api_version;
    uint16_t operations_performed;
    uint16_t proofs_ready;
    uint16_t domains_ready;
    uint16_t domains_faulted;
    uint8_t owner_ready;
    uint8_t made_progress;
    uint8_t reserved_zero[2];
} ucn_persistence_step_result_t;

typedef uint8_t ucn_persistence_request_state_t;
enum {
    UCN_PERSIST_REQUEST_QUEUED = 1,
    UCN_PERSIST_REQUEST_IN_PROGRESS = 2,
    UCN_PERSIST_REQUEST_PROOF_READY = 3,
    UCN_PERSIST_REQUEST_FAILED = 4
};

typedef struct ucn_persistence_request_view {
    uint16_t struct_size;
    uint16_t api_version;
    ucn_result_t terminal_result;
    uint8_t state;
    uint8_t provider_phase;
    uint8_t reserved_zero[2];
} ucn_persistence_request_view_t;

typedef uint8_t ucn_i_persist_work_phase_t;
enum {
    UCN_I_PERSIST_WORK_IDLE = 0,
    UCN_I_PERSIST_RECOVERY_LOAD_WITNESS = 1,
    UCN_I_PERSIST_RECOVERY_LOAD_SLOT0 = 2,
    UCN_I_PERSIST_RECOVERY_LOAD_SLOT1 = 3,
    UCN_I_PERSIST_RECOVERY_DECODE_SLOT0 = 4,
    UCN_I_PERSIST_RECOVERY_REPAIR_WITNESS = 5,
    UCN_I_PERSIST_SUBMIT_WRITE = 6,
    UCN_I_PERSIST_SUBMIT_READBACK = 7,
    UCN_I_PERSIST_SUBMIT_MARKER = 8,
    UCN_I_PERSIST_SUBMIT_WITNESS = 9,
    UCN_I_PERSIST_SUBMIT_RELOAD_WITNESS = 10,
    UCN_I_PERSIST_SUBMIT_RELOAD_SLOT0 = 11,
    UCN_I_PERSIST_SUBMIT_RELOAD_SLOT1 = 12,
    UCN_I_PERSIST_SUBMIT_SELECT = 13,
    UCN_I_PERSIST_SUBMIT_FINALIZE = 14,
    UCN_I_PERSIST_RECOVERY_DECODE_SLOT1 = 15,
    UCN_I_PERSIST_RECOVERY_SELECT = 16,
    UCN_I_PERSIST_SUBMIT_DECODE_SLOT0 = 17,
    UCN_I_PERSIST_SUBMIT_DECODE_SLOT1 = 18
};

typedef struct ucn_i_persist_record_meta {
    ucn_persist_domain_key_t domain;
    uint64_t record_generation;
    uint64_t transaction_id;
    uint32_t body_bytes;
    uint16_t schema_id;
    uint16_t schema_version;
    uint16_t operation_kind;
    uint8_t body_digest[UCN_PERSIST_DIGEST_BYTES];
} ucn_i_persist_record_meta_t;

typedef struct ucn_i_persist_hash_workspace {
    uint32_t h[8];
    uint32_t t[2];
    uint32_t v[16];
    uint8_t buffer[64];
    size_t buffered;
} ucn_i_persist_hash_workspace_t;

/* EN: All codec scratch is caller-owned and bounded. Production keeps one
 * instance inside the Persistence Owner, so hashing and record validation do
 * not consume the MCU task stack. 中文：Codec 临时量全部由调用方以固定容量持有；
 * 生产路径在 Persistence Owner 内保留一份，避免哈希和 Record 校验占用任务栈。 */
typedef struct ucn_i_persist_codec_workspace {
    ucn_i_persist_hash_workspace_t hash;
    ucn_i_persist_record_meta_t meta;
    uint8_t digest[UCN_PERSIST_DIGEST_BYTES];
    uint8_t canonical[36];
} ucn_i_persist_codec_workspace_t;

typedef struct ucn_i_persist_record_decode_request {
    const uint8_t *slot;
    const ucn_persist_manifest_entry_t *manifest;
    const uint8_t *durable_manifest_digest;
    ucn_i_persist_record_meta_t *meta_out;
    uint8_t *body_out;
    size_t slot_bytes;
    size_t body_capacity;
    uint8_t erased_value;
} ucn_i_persist_record_decode_request_t;

typedef struct ucn_i_persist_pending {
    uint8_t valid;
    uint8_t started;
    uint8_t proof_ready;
    uint8_t target_slot;
    uint8_t terminal;
    uint8_t reserved_state[3];
    uint16_t handle_generation;
    uint16_t reserved_zero;
    uint32_t caller_runtime_instance;
    uint16_t caller_owner_instance;
    uint16_t domain_generation;
    uint64_t transaction_id;
    uint64_t expected_record_generation;
    uint64_t absolute_deadline_us;
    uint64_t business_transition_digest;
    uint32_t body_bytes;
    uint16_t operation_kind;
    uint16_t reserved_zero2;
    ucn_result_t terminal_result;
    uint8_t expected_body_digest[UCN_PERSIST_DIGEST_BYTES];
    uint8_t next_body_digest[UCN_PERSIST_DIGEST_BYTES];
    ucn_handle_t volatile_continuation;
    ucn_persistence_proof_t proof;
    uint8_t body[UCN_PERSIST_BODY_BYTES];
} ucn_i_persist_pending_t;

typedef struct ucn_i_persist_domain {
    ucn_persist_manifest_entry_t manifest;
    uint64_t record_generation;
    uint64_t current_transaction_id;
    uint32_t body_bytes;
    uint16_t current_operation_kind;
    uint16_t domain_generation;
    uint16_t business_owner_instance;
    uint16_t next_handle_generation;
    uint8_t state;
    uint8_t active_slot;
    uint8_t required;
    uint8_t reserved_zero;
    uint8_t body_digest[UCN_PERSIST_DIGEST_BYTES];
    ucn_i_persist_pending_t pending;
    uint8_t body[UCN_PERSIST_BODY_BYTES];
} ucn_i_persist_domain_t;

typedef struct ucn_i_persist_io {
    uint64_t token;
    uint32_t exact_bytes;
    uint8_t active;
    uint8_t call_active;
    uint8_t phase;
    uint8_t slot_index;
    uint8_t domain_index;
    uint8_t reserved_zero[3];
} ucn_i_persist_io_t;

struct ucn_persist_callback_gate {
    uint32_t magic;
    uint16_t schema;
    uint8_t active;
    uint8_t owner_references;
    uint32_t runtime_instance;
    uint16_t owner_instance;
    uint8_t phase;
    uint8_t reserved_zero2;
    uint64_t io_token;
    uint64_t next_io_token;
    ucn_lock_ops_t lock;
};

struct ucn_persistence_owner {
    uint32_t magic;
    uint16_t schema;
    uint16_t owner_instance;
    uint32_t runtime_instance;
    uint8_t owner_phase;
    uint8_t domain_count;
    uint8_t recovery_cursor;
    uint8_t work_cursor;
    uint8_t active_domain;
    uint8_t work_phase;
    uint8_t slot_state[UCN_PERSIST_SLOT_COUNT];
    uint8_t consumer_references;
    uint8_t durable_manifest_digest[UCN_PERSIST_DIGEST_BYTES];
    ucn_lock_ops_t lock;
    ucn_persist_callback_gate_t *callback_gate;
    ucn_persistence_provider_t provider;
    ucn_i_persist_io_t io;
    ucn_persist_io_completion_t completion;
    ucn_persist_witness_view_t witness;
    ucn_i_persist_record_meta_t recovered_meta[UCN_PERSIST_SLOT_COUNT];
    ucn_result_t recovered_decode[UCN_PERSIST_SLOT_COUNT];
    ucn_i_persist_domain_t domains[UCN_PERSIST_DOMAIN_COUNT];
    uint8_t slot_buffer[UCN_PERSIST_SLOT_COUNT][UCN_PERSIST_SLOT_BYTES];
    uint8_t write_buffer[UCN_PERSIST_SLOT_BYTES];
    uint8_t readback_buffer[UCN_PERSIST_SLOT_BYTES];
    uint8_t marker_buffer[UCN_PERSIST_COMMIT_MARKER_BYTES];
    ucn_i_persist_codec_workspace_t codec_workspace;
    ucn_i_persist_record_decode_request_t codec_decode_request;
    ucn_persistence_step_result_t step_result_staging;
};

ucn_result_t ucn_i_persistence_owner_lock(ucn_persistence_owner_t *owner);
void ucn_i_persistence_owner_unlock(ucn_persistence_owner_t *owner);
uint8_t ucn_i_persistence_find_domain_index(
    const ucn_persistence_owner_t *owner,
    ucn_persist_domain_key_t key);
bool ucn_i_persistence_handle_matches(
    const ucn_persistence_owner_t *owner,
    ucn_handle_t handle,
    uint8_t *domain_index_out);
bool ucn_i_persistence_request_is_well_formed(
    const ucn_persistence_owner_t *owner,
    uint8_t domain_index,
    const ucn_persistence_request_t *request);
bool ucn_i_persistence_pending_request_equal(
    const ucn_i_persist_pending_t *pending,
    const ucn_persistence_request_t *request);
ucn_result_t ucn_i_persistence_attach_consumer(
    ucn_persistence_owner_t *owner);
ucn_result_t ucn_i_persistence_detach_consumer(
    ucn_persistence_owner_t *owner);

ucn_result_t ucn_i_persistence_start_recovery(
    ucn_persistence_owner_t *owner);
ucn_result_t ucn_i_persistence_submit(
    ucn_persistence_owner_t *owner,
    const ucn_persistence_request_t *request,
    uint64_t now_us,
    ucn_handle_t *handle_out);
ucn_result_t ucn_i_persistence_cancel(ucn_persistence_owner_t *owner,
                                      ucn_handle_t handle);
ucn_result_t ucn_i_persistence_step(
    ucn_persistence_owner_t *owner,
    uint64_t now_us,
    uint16_t operation_budget,
    ucn_persistence_step_result_t *result_out);
ucn_result_t ucn_i_persistence_domain_get(
    const ucn_persistence_owner_t *owner,
    ucn_persist_domain_key_t domain,
    ucn_persistence_domain_view_t *view_out);
ucn_result_t ucn_i_persistence_domain_copy_body(
    const ucn_persistence_owner_t *owner,
    ucn_persist_domain_key_t domain,
    uint8_t *body_out,
    size_t body_capacity,
    size_t *body_bytes_out);
ucn_result_t ucn_i_persistence_proof_get(
    const ucn_persistence_owner_t *owner,
    ucn_handle_t handle,
    ucn_persistence_proof_t *proof_out);
ucn_result_t ucn_i_persistence_request_get(
    const ucn_persistence_owner_t *owner,
    ucn_handle_t handle,
    ucn_persistence_request_view_t *view_out);
ucn_result_t ucn_i_persistence_proof_retire(
    ucn_persistence_owner_t *owner,
    ucn_handle_t handle);
ucn_result_t ucn_i_persistence_request_retire(
    ucn_persistence_owner_t *owner,
    ucn_handle_t handle);

uint32_t ucn_i_persist_crc32c(const uint8_t *bytes, size_t length);
void ucn_i_persist_blake2s128(const uint8_t *bytes,
                              size_t length,
                              uint8_t digest[UCN_PERSIST_DIGEST_BYTES],
                              ucn_i_persist_hash_workspace_t *workspace);
ucn_result_t ucn_i_persist_body_digest(
    const ucn_i_persist_record_meta_t *meta,
    const uint8_t *body,
    uint8_t digest_out[UCN_PERSIST_DIGEST_BYTES],
    ucn_i_persist_codec_workspace_t *workspace);
ucn_result_t ucn_i_persist_record_encode(
    const ucn_i_persist_record_meta_t *meta,
    const uint8_t manifest_digest[UCN_PERSIST_DIGEST_BYTES],
    const uint8_t *body,
    size_t body_capacity,
    size_t slot_bytes,
    uint8_t erased_value,
    uint8_t *slot_out,
    ucn_i_persist_codec_workspace_t *workspace);
ucn_result_t ucn_i_persist_record_decode(
    const ucn_i_persist_record_decode_request_t *request,
    ucn_i_persist_codec_workspace_t *workspace);
ucn_result_t ucn_i_persist_marker_encode(
    uint64_t record_generation,
    uint8_t marker_out[UCN_PERSIST_COMMIT_MARKER_BYTES]);
ucn_result_t ucn_i_persist_marker_decode(
    const uint8_t marker[UCN_PERSIST_COMMIT_MARKER_BYTES],
    uint64_t *record_generation_out);
bool ucn_i_persist_bytes_are_value(const uint8_t *bytes,
                                   size_t length,
                                   uint8_t value);
bool ucn_i_persist_domain_equal(ucn_persist_domain_key_t left,
                                ucn_persist_domain_key_t right);

#endif
