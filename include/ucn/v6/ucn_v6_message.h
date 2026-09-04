#ifndef UCN_V6_MESSAGE_H
#define UCN_V6_MESSAGE_H

#include "ucn/v6/ucn_v6_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_OPERATION_DIGEST_BYTES ((size_t)32U)
#define UCN_V6_OPERATION_RESULT_MAX_BYTES ((size_t)64U)
#define UCN_V6_OPERATION_JOURNAL_SLOTS ((size_t)8U)
#define UCN_V6_OPERATION_HIGH_WATER_SLOTS ((size_t)8U)
#define UCN_V6_OPERATION_JOURNAL_MAGIC UINT32_C(0x554F5036)
#define UCN_V6_OPERATION_JOURNAL_SCHEMA UINT16_C(1)

typedef enum ucn_v6_endpoint_execution_contract {
    UCN_V6_ENDPOINT_NON_RETRYABLE = 1,
    UCN_V6_ENDPOINT_IDEMPOTENT_REPLAYABLE = 2,
    UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE = 3
} ucn_v6_endpoint_execution_contract_t;

typedef enum ucn_v6_completion_level {
    UCN_V6_COMPLETION_LOCAL_ACCEPTED = 1,
    UCN_V6_COMPLETION_LINK_SUBMITTED = 2,
    UCN_V6_COMPLETION_REMOTE_REASSEMBLED = 3,
    UCN_V6_COMPLETION_REMOTE_INBOX_ACCEPTED = 4,
    UCN_V6_COMPLETION_APPLICATION_RESULT = 5
} ucn_v6_completion_level_t;

typedef struct ucn_v6_message_descriptor {
    ucn_v6_traffic_class_t traffic_class;
    ucn_v6_delivery_guarantee_t delivery_guarantee;
    ucn_v6_interaction_role_t interaction_role;
    uint16_t source_endpoint;
    uint16_t destination_endpoint;
    uint64_t operation_id;
    uint16_t payload_length;
} ucn_v6_message_descriptor_t;

typedef struct ucn_v6_endpoint_contract {
    uint16_t endpoint_id;
    uint8_t traffic_class_mask;
    uint8_t delivery_guarantee_mask;
    uint8_t interaction_role_mask;
    uint16_t max_payload_bytes;
    uint16_t max_result_bytes;
    ucn_v6_endpoint_execution_contract_t execution_contract;
} ucn_v6_endpoint_contract_t;

typedef struct ucn_v6_operation_key {
    ucn_v6_principal_t initiator_principal;
    uint64_t operation_id;
} ucn_v6_operation_key_t;

typedef enum ucn_v6_operation_phase {
    UCN_V6_OPERATION_PHASE_INVALID = 0,
    UCN_V6_OPERATION_PHASE_PREPARED = 1,
    UCN_V6_OPERATION_PHASE_EXECUTING = 2,
    UCN_V6_OPERATION_PHASE_COMMITTED_RESULT = 3,
    UCN_V6_OPERATION_PHASE_IN_DOUBT = 4,
    UCN_V6_OPERATION_PHASE_TOMBSTONED = 5
} ucn_v6_operation_phase_t;

typedef enum ucn_v6_operation_admission {
    UCN_V6_OPERATION_ADMISSION_NEW = 1,
    UCN_V6_OPERATION_ADMISSION_PREPARED = 2,
    UCN_V6_OPERATION_ADMISSION_EXECUTING = 3,
    UCN_V6_OPERATION_ADMISSION_RESULT_REPLAY = 4,
    UCN_V6_OPERATION_ADMISSION_IN_DOUBT = 5,
    UCN_V6_OPERATION_ADMISSION_TOMBSTONED = 6
} ucn_v6_operation_admission_t;

typedef struct ucn_v6_operation_slot {
    bool occupied;
    ucn_v6_operation_key_t key;
    uint16_t endpoint_id;
    ucn_v6_endpoint_execution_contract_t execution_contract;
    uint8_t request_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    ucn_v6_operation_phase_t phase;
    int32_t result_code;
    uint16_t result_length;
    uint8_t result[UCN_V6_OPERATION_RESULT_MAX_BYTES];
    uint8_t result_digest[UCN_V6_OPERATION_DIGEST_BYTES];
} ucn_v6_operation_slot_t;

typedef struct ucn_v6_operation_high_water {
    bool occupied;
    ucn_v6_principal_t initiator_principal;
    uint64_t retired_through_operation_id;
} ucn_v6_operation_high_water_t;

typedef struct ucn_v6_operation_journal_snapshot {
    uint32_t magic;
    uint16_t schema;
    uint16_t slot_count;
    uint64_t snapshot_generation;
    ucn_v6_operation_slot_t slots[UCN_V6_OPERATION_JOURNAL_SLOTS];
    ucn_v6_operation_high_water_t
        high_waters[UCN_V6_OPERATION_HIGH_WATER_SLOTS];
} ucn_v6_operation_journal_snapshot_t;

typedef struct ucn_v6_message_store_ops {
    void *context;
    ucn_v6_result_t (*load_journal)(
        void *context,
        ucn_v6_operation_journal_snapshot_t *snapshot);
    ucn_v6_result_t (*submit_journal)(
        void *context,
        const ucn_v6_operation_journal_snapshot_t *snapshot);
    ucn_v6_result_t (*load_operation_id_high_water)(
        void *context,
        uint64_t *high_water);
    ucn_v6_result_t (*persist_operation_id_high_water)(
        void *context,
        uint64_t high_water);
} ucn_v6_message_store_ops_t;

typedef struct ucn_v6_operation_id_allocator {
    ucn_v6_message_store_ops_t store;
    ucn_v6_callback_gate_t *callback_gate;
    uint64_t next_id;
    uint64_t reserved_through;
    uint32_t reservation_block_size;
    bool initialized;
    bool faulted;
} ucn_v6_operation_id_allocator_t;

typedef struct ucn_v6_operation_journal {
    ucn_v6_operation_journal_snapshot_t committed;
    ucn_v6_message_store_ops_t store;
    ucn_v6_callback_gate_t *callback_gate;
    bool initialized;
    bool faulted;
} ucn_v6_operation_journal_t;

/* EN: Validates orthogonal Traffic, delivery and interaction fields against
 * one immutable Endpoint contract.
 * 中文：按不可变 Endpoint 合同校验彼此正交的流量、交付和交互字段。 */
ucn_v6_result_t ucn_v6_message_validate(
    const ucn_v6_message_descriptor_t *message,
    const ucn_v6_endpoint_contract_t *endpoint);

/* EN: Initializes a persistently reserved, non-wrapping Operation-ID source.
 * 中文：初始化持久区间预留且不可回绕的 Operation ID 分配器。 */
ucn_v6_result_t ucn_v6_operation_id_allocator_init(
    ucn_v6_operation_id_allocator_t *allocator,
    const ucn_v6_message_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate,
    uint32_t reservation_block_size);
ucn_v6_result_t ucn_v6_operation_id_take(
    ucn_v6_operation_id_allocator_t *allocator,
    uint64_t *operation_id);

/* EN: Loads the fixed journal and atomically migrates crash-time EXECUTING
 * entries to IN_DOUBT before accepting work.
 * 中文：加载固定 Journal，并在接收工作前原子地把掉电时 EXECUTING 项迁移为
 * IN_DOUBT。 */
ucn_v6_result_t ucn_v6_operation_journal_init(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_message_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate);
ucn_v6_result_t ucn_v6_operation_prepare(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    uint16_t endpoint_id,
    ucn_v6_endpoint_execution_contract_t execution_contract,
    const uint8_t request_digest[UCN_V6_OPERATION_DIGEST_BYTES],
    ucn_v6_operation_admission_t *admission);
ucn_v6_result_t ucn_v6_operation_mark_executing(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key);
ucn_v6_result_t ucn_v6_operation_commit_result(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    int32_t result_code,
    const uint8_t *result,
    uint16_t result_length,
    const uint8_t result_digest[UCN_V6_OPERATION_DIGEST_BYTES]);
ucn_v6_result_t ucn_v6_operation_resolve_in_doubt(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    bool authenticated,
    bool result_known,
    int32_t result_code,
    const uint8_t *result,
    uint16_t result_length,
    const uint8_t terminal_digest[UCN_V6_OPERATION_DIGEST_BYTES]);
ucn_v6_result_t ucn_v6_operation_abort_prepared(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    int32_t result_code,
    const uint8_t terminal_digest[UCN_V6_OPERATION_DIGEST_BYTES]);
ucn_v6_result_t ucn_v6_operation_tombstone_result(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    bool authenticated_result_ack,
    bool minimum_retention_elapsed);
ucn_v6_result_t ucn_v6_operation_reclaim_tombstone(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    uint64_t durable_initiator_high_water,
    bool maximum_replay_lifetime_elapsed);
ucn_v6_result_t ucn_v6_operation_copy_slot(
    const ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    ucn_v6_operation_slot_t *slot);

#ifdef __cplusplus
}
#endif

#endif
