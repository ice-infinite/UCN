#ifndef UCN_CLUSTER_CONFIG_TX_H
#define UCN_CLUSTER_CONFIG_TX_H

/* CLV2-07-02: one bounded Config transaction value model.
 *
 * This type is intentionally independent of Cluster RX/TX/FSM. It records
 * enough state for later Config persistence and Joint quorum logic but does
 * not itself persist, send, acknowledge on wire or change membership. */

#include "ucn/ucn_cluster_config_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ucn_cluster_config_proposal_kind {
    UCN_CLUSTER_CONFIG_PROPOSAL_INVALID = 0,
    UCN_CLUSTER_CONFIG_PROPOSAL_ADD = 1,
    UCN_CLUSTER_CONFIG_PROPOSAL_REMOVE = 2
} ucn_cluster_config_proposal_kind_t;

typedef enum ucn_cluster_config_tx_phase {
    UCN_CLUSTER_CONFIG_TX_PHASE_INVALID = 0,
    UCN_CLUSTER_CONFIG_TX_PHASE_IDLE = 1,
    UCN_CLUSTER_CONFIG_TX_PHASE_PROPOSING = 2,
    UCN_CLUSTER_CONFIG_TX_PHASE_PREPARED = 3,
    UCN_CLUSTER_CONFIG_TX_PHASE_JOINT = 4,
    UCN_CLUSTER_CONFIG_TX_PHASE_COMMITTED = 5,
    UCN_CLUSTER_CONFIG_TX_PHASE_ABORTED = 6
} ucn_cluster_config_tx_phase_t;

typedef enum ucn_cluster_config_persist_stage {
    UCN_CLUSTER_CONFIG_PERSIST_STAGE_INVALID = 0,
    UCN_CLUSTER_CONFIG_PERSIST_STAGE_NONE = 1,
    UCN_CLUSTER_CONFIG_PERSIST_STAGE_PREPARE_PENDING = 2,
    UCN_CLUSTER_CONFIG_PERSIST_STAGE_PREPARED = 3,
    UCN_CLUSTER_CONFIG_PERSIST_STAGE_JOINT_PENDING = 4,
    UCN_CLUSTER_CONFIG_PERSIST_STAGE_JOINT_DURABLE = 5,
    UCN_CLUSTER_CONFIG_PERSIST_STAGE_COMMIT_PENDING = 6,
    UCN_CLUSTER_CONFIG_PERSIST_STAGE_COMMITTED = 7,
    UCN_CLUSTER_CONFIG_PERSIST_STAGE_ABORT_PENDING = 8,
    UCN_CLUSTER_CONFIG_PERSIST_STAGE_ABORTED = 9
} ucn_cluster_config_persist_stage_t;

typedef struct ucn_cluster_config_tx {
    uint32_t transaction_id;
    ucn_node_id_t proposal_node_id;
    uint32_t deadline_ms;
    uint32_t retry_due_ms;
    uint64_t old_ack_bitmap;
    uint64_t new_ack_bitmap;
    uint8_t phase;
    uint8_t proposal_kind;
    uint8_t persist_stage;
    uint8_t retry_count;
    ucn_cluster_config_state_t base_config;
    ucn_cluster_config_state_t proposed_config;
} ucn_cluster_config_tx_t;

void ucn_cluster_config_tx_init_empty(ucn_cluster_config_tx_t *tx);
bool ucn_cluster_config_proposal_kind_is_valid(
    ucn_cluster_config_proposal_kind_t kind);
bool ucn_cluster_config_tx_phase_is_valid(ucn_cluster_config_tx_phase_t phase);
bool ucn_cluster_config_persist_stage_is_valid(
    ucn_cluster_config_persist_stage_t stage);
bool ucn_cluster_config_tx_is_valid(const ucn_cluster_config_tx_t *tx);
bool ucn_cluster_config_tx_is_active(const ucn_cluster_config_tx_t *tx);

/* Starts the only allowed active transaction. Output must be canonical IDLE
 * or active; an active record is never overwritten. */
ucn_result_t ucn_cluster_config_tx_begin(
    ucn_cluster_config_tx_t *tx,
    uint32_t transaction_id,
    ucn_cluster_config_proposal_kind_t proposal_kind,
    ucn_node_id_t proposal_node_id,
    const ucn_cluster_config_state_t *base_config,
    const ucn_cluster_config_state_t *proposed_joint,
    uint32_t deadline_ms);

/* Records a voter ACK against both C_old/C_new when the node belongs to both.
 * Duplicate ACKs are idempotent; a non-voter leaves tx unchanged. */
ucn_result_t ucn_cluster_config_tx_record_ack(ucn_cluster_config_tx_t *tx,
                                              ucn_node_id_t voter_node_id);
ucn_result_t ucn_cluster_config_tx_schedule_retry(
    ucn_cluster_config_tx_t *tx,
    uint32_t retry_due_ms);
ucn_result_t ucn_cluster_config_tx_set_persist_stage(
    ucn_cluster_config_tx_t *tx,
    ucn_cluster_config_persist_stage_t stage);
bool ucn_cluster_config_tx_is_expired(const ucn_cluster_config_tx_t *tx,
                                      uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
