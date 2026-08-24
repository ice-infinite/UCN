#ifndef UCN_CLUSTER_STORAGE_H
#define UCN_CLUSTER_STORAGE_H

#include "ucn/ucn_cluster.h"

/* Fixed MCU storage for the single Protocol Owner that owns a Cluster.
 * Application tasks and pointer-only users include ucn_cluster.h instead.
 * These fields are private implementation storage, not an application ABI. */
#define UCN_CLUSTER_STORAGE_LAYOUT_VERSION UINT32_C(2)

struct ucn_cluster {
    ucn_cluster_config_t config;
    bool persistence_pending;
    bool persistence_faulted;
    bool persistence_retry_pending;
    bool persistence_io_active;
    bool persistence_retry_dispatch;
    uint8_t persistence_pending_action;
    uint8_t persistence_pending_operation;
    uint8_t persistence_retry_action;
    ucn_result_t persistence_failure;
    ucn_node_id_t persistence_pending_destination;
    ucn_node_id_t persistence_retry_destination;
    uint32_t persistence_pending_token;
    uint32_t persistence_pending_operation_id;
    uint32_t persistence_pending_fingerprint;
    ucn_cluster_phase_t phase;
    uint32_t cluster_id;
    uint32_t term;
    ucn_node_id_t head_node_id;
    uint32_t last_cluster_id;
    uint32_t max_seen_term;
    ucn_node_id_t last_stable_head;
    uint32_t cluster_id_round;
    uint16_t current_head_score;
    uint32_t observation_deadline_ms;
    uint32_t role_since_ms;
    uint32_t election_deadline_ms;
    uint32_t head_lease_expires_at_ms;
    uint32_t head_grace_deadline_ms;
    uint32_t next_advertise_ms;
    uint32_t next_join_retry_ms;
    uint32_t next_keepalive_ms;
    uint32_t next_nonce;
    uint8_t advertise_cursor;
    ucn_node_id_t pending_head_node_id;
    uint32_t pending_cluster_id;
    uint32_t pending_term;
    uint16_t pending_head_score;
    ucn_cluster_peer_t peers[UCN_CLUSTER_MAX_PEERS];
    ucn_cluster_candidate_t candidates[UCN_CLUSTER_MAX_CANDIDATES];
    ucn_cluster_member_table_t primary_members;
    ucn_cluster_voter_set_t active_voter_set;
    ucn_node_id_t backup_node_id;
    uint32_t backup_generation;
    uint32_t membership_sequence;
    uint8_t backup_assign_cursor;
    uint8_t backup_assign_remaining;
    uint32_t next_backup_assign_ms;
    ucn_node_id_t backup_primary_node_id;
    uint32_t backup_sync_cursor;
    uint32_t next_backup_heartbeat_ms;
    uint32_t next_backup_sync_ms;
    uint8_t backup_delta_cursor;
    uint32_t next_backup_delta_ms;
    uint32_t backup_resync_deadline_ms;
    uint32_t backup_primary_deadline_ms;
    uint32_t backup_primary_lease_deadline_ms;
    uint8_t backup_missed_heartbeats;
    uint32_t backup_takeover_deadline_ms;
    uint8_t backup_takeover_ack_count;
    uint32_t backup_takeover_acked;
    uint8_t backup_takeover_prepare_cursor;
    uint8_t backup_takeover_announce_cursor;
    uint8_t backup_takeover_announce_remaining;
    bool backup_takeover_announce_active;
    ucn_node_id_t known_backup_node_id;
    uint32_t known_backup_generation;
    uint32_t pending_join_nonce;
    uint32_t last_stepdown_nonce;
    uint32_t backup_candidate_cooldown_until_ms;
    ucn_node_id_t backup_rejected_node_id;
    uint32_t member_voted_term;
    uint32_t member_voted_cluster_id;
    uint32_t member_voted_generation;
    ucn_cluster_token_bucket_t token_bucket;
    uint32_t stepdown_deadline_ms;
    uint32_t recovery_cluster_id;
    uint32_t recovery_deadline_ms;
    uint32_t recovery_cooldown_until_ms;
    uint32_t recovery_backoff_deadline_ms;
    uint32_t recovery_nonce;
    uint32_t accepted_recovery_nonce;
    ucn_node_id_t known_recovery_source;
    uint8_t recovery_ack_count;
    uint32_t recovery_acked;
    uint32_t parent_cluster_id;
    uint32_t parent_term;
    uint32_t parent_config_id;
    uint32_t recovery_round;
    uint32_t lineage_config_binding;
    uint32_t lineage_reset_deadline_ms;
    ucn_cluster_stats_t stats;
    ucn_cluster_transition_reason_t transition_reason;
    uint32_t transition_count;
    bool authority_active;
    ucn_cluster_phase_t authority_phase;
    ucn_cluster_authority_fence_reason_t authority_fence_reason;
    ucn_cluster_phase_t head_resume_phase;
    uint32_t quorum_loss_deadline_ms;
    uint32_t quorum_restore_since_ms;
    uint32_t fenced_dissolve_deadline_ms;
    struct ucn_cluster_authority_runtime *authority_runtime;
};

typedef char ucn_cluster_storage_budget_must_hold[
    sizeof(struct ucn_cluster) <= UCN_CLUSTER_STORAGE_BUDGET_BYTES ? 1 : -1];

#endif
