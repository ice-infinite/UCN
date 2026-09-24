#ifndef UCN_INTERNAL_CLUSTER_H
#define UCN_INTERNAL_CLUSTER_H

#include "internal/ucn_digest.h"
#include "internal/ucn_owner.h"
#include "ucn/ucn_config.h"

#define UCN_I_CLUSTER_MAGIC UINT32_C(0x434C5336)
#define UCN_I_CLUSTER_SCHEMA UINT16_C(1)
#define UCN_I_CLUSTER_RECORD_SCHEMA_ID UINT16_C(0x0803)
#define UCN_I_CLUSTER_RECORD_SCHEMA UINT16_C(2)
#define UCN_I_CLUSTER_DIGEST_BYTES 16U
#define UCN_I_CLUSTER_PRINCIPAL_BYTES 16U
#define UCN_I_CLUSTER_RECORD_HEADER_BYTES 144U
#define UCN_I_CLUSTER_CONFIG_MEMBER_BYTES 24U
#define UCN_I_CLUSTER_VOTE_EVIDENCE_BYTES 32U
#define UCN_I_CLUSTER_SNAPSHOT_HEADER_BYTES 40U
#define UCN_I_CLUSTER_SNAPSHOT_MEMBER_BYTES 56U
#define UCN_I_CLUSTER_STEP_BUDGET_DEFAULT 4U

#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_I_CLUSTER_CONFIG_MEMBER_COUNT 1U
#define UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT 4U
#define UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT 1U
#define UCN_I_CLUSTER_TUNNEL_SLOT_COUNT 1U
#define UCN_I_CLUSTER_PERSIST_BODY_LIMIT 256U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_I_CLUSTER_CONFIG_MEMBER_COUNT 3U
#define UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT 8U
#define UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT 2U
#define UCN_I_CLUSTER_TUNNEL_SLOT_COUNT 2U
#define UCN_I_CLUSTER_PERSIST_BODY_LIMIT 512U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define UCN_I_CLUSTER_CONFIG_MEMBER_COUNT 7U
#define UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT 16U
#define UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT 4U
#define UCN_I_CLUSTER_TUNNEL_SLOT_COUNT 4U
#define UCN_I_CLUSTER_PERSIST_BODY_LIMIT 1024U
#else
#error "UCN_PROFILE must select NANO, LITE, or FULL"
#endif

#define UCN_I_CLUSTER_RECORD_BYTES                                      \
    (UCN_I_CLUSTER_RECORD_HEADER_BYTES +                                \
     2U * UCN_I_CLUSTER_CONFIG_MEMBER_COUNT *                           \
         UCN_I_CLUSTER_CONFIG_MEMBER_BYTES +                            \
     2U * UCN_I_CLUSTER_CONFIG_MEMBER_COUNT *                           \
         UCN_I_CLUSTER_VOTE_EVIDENCE_BYTES)

#define UCN_I_CLUSTER_SNAPSHOT_CANONICAL_BYTES                          \
    (UCN_I_CLUSTER_SNAPSHOT_HEADER_BYTES +                              \
     UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT *                               \
         UCN_I_CLUSTER_SNAPSHOT_MEMBER_BYTES)

UCN_STATIC_ASSERT(UCN_I_CLUSTER_CONFIG_MEMBER_COUNT <= 32U,
                  cluster_config_bitmap_must_fit_u32);
UCN_STATIC_ASSERT(UCN_I_CLUSTER_RECORD_BYTES <=
                      UCN_I_CLUSTER_PERSIST_BODY_LIMIT,
                  cluster_record_must_fit_persistence_body);
UCN_STATIC_ASSERT(UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT +
                      UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT +
                      UCN_I_CLUSTER_TUNNEL_SLOT_COUNT + 1U <= UINT8_MAX,
                  cluster_step_cursor_must_fit_u8);

typedef uint8_t ucn_i_cluster_role_t;
enum {
    UCN_I_CLUSTER_OBSERVER = 0,
    UCN_I_CLUSTER_MEMBER = 1,
    UCN_I_CLUSTER_VOTER = 2,
    UCN_I_CLUSTER_BACKUP = 3,
    UCN_I_CLUSTER_HEAD = 4,
    UCN_I_CLUSTER_FENCED = 5
};

typedef uint8_t ucn_i_cluster_phase_t;
enum {
    UCN_I_CLUSTER_STABLE = 1,
    UCN_I_CLUSTER_PREPARED = 2,
    UCN_I_CLUSTER_JOINT = 3,
    UCN_I_CLUSTER_COLLECTING = 4,
    UCN_I_CLUSTER_QUORUM = 5,
    UCN_I_CLUSTER_EPOCH_DURABLE = 6,
    UCN_I_CLUSTER_ABORTED = 7,
    UCN_I_CLUSTER_FAULT = 8
};

typedef uint8_t ucn_i_cluster_transition_kind_t;
enum {
    UCN_I_CLUSTER_TRANSITION_NONE = 0,
    UCN_I_CLUSTER_TRANSITION_CONFIG = 1,
    UCN_I_CLUSTER_TRANSITION_TAKEOVER = 2,
    UCN_I_CLUSTER_TRANSITION_RECOVERY = 3,
    UCN_I_CLUSTER_TRANSITION_HANDOVER = 4,
    UCN_I_CLUSTER_TRANSITION_MERGE = 5
};

typedef uint16_t ucn_i_cluster_operation_kind_t;
enum {
    UCN_I_CLUSTER_PERSIST_CREATE_COMMIT = UINT16_C(0x0830),
    UCN_I_CLUSTER_PERSIST_CONFIG_PREPARED = UINT16_C(0x0831),
    UCN_I_CLUSTER_PERSIST_CONFIG_JOINT = UINT16_C(0x0832),
    UCN_I_CLUSTER_PERSIST_CONFIG_COMMIT = UINT16_C(0x0833),
    UCN_I_CLUSTER_PERSIST_VOTE = UINT16_C(0x0834),
    UCN_I_CLUSTER_PERSIST_EPOCH_COMMIT = UINT16_C(0x0835),
    UCN_I_CLUSTER_PERSIST_HANDOVER_FENCE = UINT16_C(0x0836),
    UCN_I_CLUSTER_PERSIST_ABORT = UINT16_C(0x0837),
    UCN_I_CLUSTER_PERSIST_BACKUP_READY = UINT16_C(0x0838),
    UCN_I_CLUSTER_PERSIST_REKEY_COMMIT = UINT16_C(0x0839),
    UCN_I_CLUSTER_PERSIST_MERGE_RETIRE = UINT16_C(0x083A)
};

enum {
    UCN_I_CLUSTER_MEMBER_FLAG_MEMBER = 1U << 0,
    UCN_I_CLUSTER_MEMBER_FLAG_VOTER = 1U << 1,
    UCN_I_CLUSTER_MEMBER_FLAG_BACKUP = 1U << 2
};

typedef struct ucn_i_cluster_epoch {
    uint32_t cluster_id;
    uint32_t term;
    uint32_t head_binding_generation;
    uint8_t head_principal[UCN_I_CLUSTER_PRINCIPAL_BYTES];
} ucn_i_cluster_epoch_t;

typedef struct ucn_i_cluster_config_member {
    uint32_t binding_generation;
    uint8_t principal[UCN_I_CLUSTER_PRINCIPAL_BYTES];
    uint8_t flags;
    uint8_t reserved_zero[3];
} ucn_i_cluster_config_member_t;

typedef struct ucn_i_cluster_config_view {
    uint32_t config_id;
    uint32_t generation;
    uint8_t member_count;
    uint8_t reserved_zero[3];
    ucn_i_cluster_config_member_t
        members[UCN_I_CLUSTER_CONFIG_MEMBER_COUNT];
} ucn_i_cluster_config_view_t;

typedef struct ucn_i_cluster_member_fact {
    uint64_t lease_deadline_us;
    uint64_t capability_deadline_us;
    uint32_t binding_generation;
    uint32_t session_generation;
    uint32_t capability_generation;
    uint32_t route_generation;
    uint32_t link_generation;
    uint16_t link_id;
    uint8_t authenticated;
    uint8_t current;
    uint8_t backup_eligible;
    uint8_t reserved_zero;
    uint8_t principal[UCN_I_CLUSTER_PRINCIPAL_BYTES];
    uint8_t capability_digest[UCN_I_CLUSTER_DIGEST_BYTES];
} ucn_i_cluster_member_fact_t;

typedef struct ucn_i_cluster_member_slot {
    ucn_i_cluster_member_fact_t fact;
    uint8_t occupied;
    uint8_t reserved_zero[7];
} ucn_i_cluster_member_slot_t;

typedef struct ucn_i_cluster_vote_evidence {
    uint32_t binding_generation;
    uint32_t session_generation;
    uint32_t capability_generation;
    uint32_t vote_id;
    uint8_t canonical_digest[UCN_I_CLUSTER_DIGEST_BYTES];
    uint8_t valid;
    uint8_t voter_index;
    uint8_t reserved_zero[2];
    uint8_t principal[UCN_I_CLUSTER_PRINCIPAL_BYTES];
} ucn_i_cluster_vote_evidence_t;

typedef struct ucn_i_cluster_handover_ready {
    uint64_t transaction_id;
    uint64_t lease_deadline_us;
    uint32_t target_cluster_id;
    uint32_t target_term;
    uint32_t target_binding_generation;
    uint32_t target_config_id;
    uint32_t target_config_generation;
    uint32_t capability_generation;
    uint32_t authority_generation;
    uint8_t target_principal[UCN_I_CLUSTER_PRINCIPAL_BYTES];
    uint8_t proof_digest[UCN_I_CLUSTER_DIGEST_BYTES];
    uint8_t authenticated;
    uint8_t quorum_verified;
    uint8_t durable_continuation;
    uint8_t reserved_zero;
} ucn_i_cluster_handover_ready_t;

typedef struct ucn_i_cluster_transition {
    ucn_i_cluster_epoch_t target_epoch;
    ucn_i_cluster_config_view_t target_config;
    ucn_i_cluster_handover_ready_t handover_ready;
    uint64_t transaction_id;
    uint64_t absolute_deadline_us;
    uint32_t old_vote_mask;
    uint32_t new_vote_mask;
    uint8_t kind;
    uint8_t phase;
    uint8_t handover_ready_valid;
    uint8_t reserved_zero;
    ucn_i_cluster_vote_evidence_t
        old_votes[UCN_I_CLUSTER_CONFIG_MEMBER_COUNT];
    ucn_i_cluster_vote_evidence_t
        new_votes[UCN_I_CLUSTER_CONFIG_MEMBER_COUNT];
} ucn_i_cluster_transition_t;

typedef struct ucn_i_cluster_state {
    ucn_i_cluster_epoch_t epoch;
    ucn_i_cluster_config_view_t stable_config;
    ucn_i_cluster_transition_t transition;
    uint64_t transaction_high_water;
    uint32_t lineage_generation;
    uint32_t retired_cluster_high_water;
    uint32_t backup_assignment_generation;
    uint32_t backup_coverage_mask;
    uint8_t backup_snapshot_digest[UCN_I_CLUSTER_DIGEST_BYTES];
    uint8_t role;
    uint8_t phase;
    uint8_t joint_valid;
    uint8_t authority_fenced;
    uint8_t backup_ready;
    uint8_t reserved_zero[3];
} ucn_i_cluster_state_t;

typedef struct ucn_i_cluster_snapshot_header {
    uint64_t absolute_deadline_us;
    uint32_t cluster_id;
    uint32_t term;
    uint32_t config_id;
    uint32_t config_generation;
    uint32_t assignment_generation;
    uint32_t snapshot_sequence;
    uint32_t protected_voter_bitmap;
    uint8_t member_count;
    uint8_t reserved_zero[3];
    uint8_t expected_digest[UCN_I_CLUSTER_DIGEST_BYTES];
} ucn_i_cluster_snapshot_header_t;

typedef struct ucn_i_cluster_snapshot_member {
    uint32_t binding_generation;
    uint32_t session_generation;
    uint32_t capability_generation;
    uint32_t route_generation;
    uint32_t link_generation;
    uint16_t link_id;
    uint8_t flags;
    uint8_t reserved_zero;
    uint8_t principal[UCN_I_CLUSTER_PRINCIPAL_BYTES];
    uint8_t capability_digest[UCN_I_CLUSTER_DIGEST_BYTES];
} ucn_i_cluster_snapshot_member_t;

typedef struct ucn_i_cluster_snapshot_buffer {
    ucn_i_cluster_snapshot_header_t header;
    uint8_t canonical[UCN_I_CLUSTER_SNAPSHOT_CANONICAL_BYTES];
    uint8_t received_count;
    uint8_t valid;
    uint8_t reserved_zero[6];
} ucn_i_cluster_snapshot_buffer_t;

typedef struct ucn_i_cluster_snapshot_sync {
    ucn_i_cluster_snapshot_buffer_t buffers[2];
    uint8_t active_index;
    uint8_t building_index;
    uint8_t building;
    uint8_t reserved_zero[5];
} ucn_i_cluster_snapshot_sync_t;

typedef struct ucn_i_cluster_directory_fact {
    ucn_i_cluster_epoch_t remote_epoch;
    uint64_t origin_sequence;
    uint64_t authority_deadline_us;
    uint64_t capability_deadline_us;
    uint64_t flow_deadline_us;
    uint32_t remote_config_id;
    uint32_t remote_config_generation;
    uint32_t authority_generation;
    uint32_t source_session_generation;
    uint32_t capability_generation;
    uint32_t route_generation;
    uint32_t path_generation;
    uint32_t link_generation;
    uint16_t path_id;
    uint16_t link_id;
    uint8_t capability_digest[UCN_I_CLUSTER_DIGEST_BYTES];
    uint8_t authority_digest[UCN_I_CLUSTER_DIGEST_BYTES];
    uint8_t authenticated;
    uint8_t quorum_verified;
    uint8_t flow_active;
    uint8_t reserved_zero;
} ucn_i_cluster_directory_fact_t;

typedef struct ucn_i_cluster_directory_slot {
    ucn_i_cluster_directory_fact_t fact;
    uint8_t occupied;
    uint8_t reserved_zero[7];
} ucn_i_cluster_directory_slot_t;

typedef struct ucn_i_cluster_flow_fact {
    uint64_t deadline_us;
    uint32_t source_binding_generation;
    uint32_t source_session_generation;
    uint32_t destination_binding_generation;
    uint32_t destination_session_generation;
    uint32_t capability_generation;
    uint32_t route_generation;
    uint32_t path_generation;
    uint32_t link_generation;
    uint16_t path_id;
    uint16_t link_id;
    uint8_t source_principal[UCN_I_CLUSTER_PRINCIPAL_BYTES];
    uint8_t destination_principal[UCN_I_CLUSTER_PRINCIPAL_BYTES];
    uint8_t capability_digest[UCN_I_CLUSTER_DIGEST_BYTES];
    uint8_t active;
    uint8_t authenticated;
    uint8_t reserved_zero[6];
} ucn_i_cluster_flow_fact_t;

typedef struct ucn_i_cluster_tunnel_request {
    uint64_t tunnel_id;
    uint64_t directory_origin_sequence;
    uint64_t absolute_deadline_us;
    uint32_t source_cluster_id;
    uint32_t destination_cluster_id;
    ucn_i_cluster_flow_fact_t flow;
} ucn_i_cluster_tunnel_request_t;

typedef struct ucn_i_cluster_tunnel_slot {
    ucn_i_cluster_tunnel_request_t value;
    uint8_t occupied;
    uint8_t reserved_zero[7];
} ucn_i_cluster_tunnel_slot_t;

typedef struct ucn_i_cluster_config {
    uint32_t runtime_instance;
    uint16_t owner_instance;
    uint16_t reserved_zero;
    uint8_t local_principal[UCN_I_CLUSTER_PRINCIPAL_BYTES];
    ucn_i_lock_ops_t state_lock;
} ucn_i_cluster_config_t;

typedef struct ucn_i_cluster_durability {
    uint64_t domain_id;
    uint64_t foundation_transaction_id;
    uint64_t expected_record_generation;
    uint64_t absolute_deadline_us;
    ucn_handle_t volatile_continuation;
    uint16_t persistence_domain_generation;
    uint16_t schema_id;
    uint16_t schema_version;
} ucn_i_cluster_durability_t;

typedef struct ucn_i_cluster_requirement {
    ucn_i_cluster_durability_t durability;
    uint8_t body[UCN_I_CLUSTER_RECORD_BYTES];
    uint8_t canonical_body_digest[UCN_I_CLUSTER_DIGEST_BYTES];
    uint8_t expected_body_digest[UCN_I_CLUSTER_DIGEST_BYTES];
    uint32_t runtime_instance;
    uint32_t body_bytes;
    uint16_t caller_owner_instance;
    uint16_t operation_kind;
} ucn_i_cluster_requirement_t;

typedef struct ucn_i_cluster_proof {
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
    uint8_t body_digest[UCN_I_CLUSTER_DIGEST_BYTES];
} ucn_i_cluster_proof_t;

typedef struct ucn_i_cluster_authority_view {
    ucn_i_cluster_epoch_t epoch;
    uint32_t stable_config_id;
    uint32_t stable_config_generation;
    uint32_t joint_config_id;
    uint32_t joint_config_generation;
    uint32_t live_old_voters;
    uint32_t live_new_voters;
    uint8_t role;
    uint8_t phase;
    uint8_t authority_active;
    uint8_t joint_valid;
    uint8_t fenced;
    uint8_t reserved_zero[3];
} ucn_i_cluster_authority_view_t;

typedef struct ucn_i_cluster_step_result {
    uint16_t members_expired;
    uint16_t votes_invalidated;
    uint16_t directories_expired;
    uint16_t tunnels_expired;
    uint16_t operations;
    uint16_t snapshots_aborted;
    uint8_t authority_active;
    uint8_t fenced;
    uint8_t made_progress;
    uint8_t reserved_zero;
} ucn_i_cluster_step_result_t;

typedef struct ucn_i_cluster_owner {
    uint32_t magic;
    uint32_t runtime_instance;
    uint16_t schema;
    uint16_t owner_instance;
    uint8_t local_principal[UCN_I_CLUSTER_PRINCIPAL_BYTES];
    ucn_i_lock_ops_t state_lock;
    ucn_i_cluster_state_t state;
    ucn_i_cluster_state_t pending_state;
    ucn_i_cluster_member_slot_t members[UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT];
    ucn_i_cluster_snapshot_sync_t snapshot_sync;
    ucn_i_cluster_directory_slot_t
        directory[UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT];
    ucn_i_cluster_tunnel_slot_t tunnels[UCN_I_CLUSTER_TUNNEL_SLOT_COUNT];
    ucn_i_cluster_durability_t pending_durability;
    ucn_handle_t persistence_handle;
    uint16_t pending_operation_kind;
    uint8_t pending_valid;
    uint8_t persistence_bound;
    uint8_t authority_active;
    uint8_t step_cursor;
    uint8_t pending_body_digest[UCN_I_CLUSTER_DIGEST_BYTES];
    uint8_t current_body_digest[UCN_I_CLUSTER_DIGEST_BYTES];
    ucn_i_sha256_workspace_t hash_workspace;
} ucn_i_cluster_owner_t;

ucn_result_t ucn_i_cluster_owner_init(
    ucn_i_cluster_owner_t *owner, const ucn_i_cluster_config_t *config);
ucn_result_t ucn_i_cluster_owner_reset(ucn_i_cluster_owner_t *owner);
ucn_result_t ucn_i_cluster_member_observe(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_member_fact_t *fact, uint64_t now_us);
ucn_result_t ucn_i_cluster_authority_preflight(
    ucn_i_cluster_owner_t *owner, uint64_t now_us,
    ucn_i_cluster_authority_view_t *view_out);
ucn_result_t ucn_i_cluster_get_view(
    ucn_i_cluster_owner_t *owner,
    ucn_i_cluster_authority_view_t *view_out);

ucn_result_t ucn_i_cluster_create_prepare(
    ucn_i_cluster_owner_t *owner, const ucn_i_cluster_epoch_t *epoch,
    const ucn_i_cluster_config_view_t *config, uint8_t local_role,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out);
ucn_result_t ucn_i_cluster_config_prepare(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_config_view_t *target_config,
    uint64_t transaction_id, uint64_t now_us,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out);
ucn_result_t ucn_i_cluster_config_enter_joint_prepare(
    ucn_i_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us, const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out);
ucn_result_t ucn_i_cluster_config_commit_prepare(
    ucn_i_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us, const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out);
ucn_result_t ucn_i_cluster_config_abort_prepare(
    ucn_i_cluster_owner_t *owner, uint64_t transaction_id,
    uint32_t target_config_id, uint32_t target_config_generation,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out);

ucn_result_t ucn_i_cluster_backup_ready_prepare(
    ucn_i_cluster_owner_t *owner, uint32_t assignment_generation,
    const ucn_i_cluster_config_view_t *snapshot_config,
    uint32_t protected_voter_bitmap,
    const uint8_t snapshot_digest[UCN_I_CLUSTER_DIGEST_BYTES],
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out);
ucn_result_t ucn_i_cluster_transition_begin(
    ucn_i_cluster_owner_t *owner, uint8_t kind,
    const ucn_i_cluster_epoch_t *target_epoch,
    const ucn_i_cluster_config_view_t *target_config,
    uint64_t transaction_id, uint64_t absolute_deadline_us,
    uint64_t now_us);
ucn_result_t ucn_i_cluster_transition_vote_prepare(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_vote_evidence_t *vote, bool target_config_vote,
    uint64_t now_us, const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out);
ucn_result_t ucn_i_cluster_handover_ready_accept(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_handover_ready_t *ready, uint64_t now_us);
ucn_result_t ucn_i_cluster_transition_commit_prepare(
    ucn_i_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us, const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out);
ucn_result_t ucn_i_cluster_handover_continuation_preflight(
    ucn_i_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us);

ucn_result_t ucn_i_cluster_snapshot_begin(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_snapshot_header_t *header, uint64_t now_us);
ucn_result_t ucn_i_cluster_snapshot_member(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_snapshot_member_t *member, uint64_t now_us);
ucn_result_t ucn_i_cluster_snapshot_end(
    ucn_i_cluster_owner_t *owner, uint64_t now_us);
ucn_result_t ucn_i_cluster_backup_ready_from_mirror_prepare(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out);

ucn_result_t ucn_i_cluster_rekey_prepare(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_epoch_t *successor_epoch,
    const ucn_i_cluster_config_view_t *successor_config,
    uint64_t transaction_id, uint64_t now_us,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out);
ucn_result_t ucn_i_cluster_merge_retire_prepare(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_epoch_t *successor_epoch,
    const ucn_i_cluster_config_view_t *successor_config,
    const ucn_i_cluster_handover_ready_t *ready,
    uint64_t transaction_id, uint64_t now_us,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out);
ucn_result_t ucn_i_cluster_merge_ready_accept(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_handover_ready_t *ready, uint64_t now_us);
ucn_result_t ucn_i_cluster_merge_continuation_preflight(
    ucn_i_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us);

ucn_result_t ucn_i_cluster_directory_install(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_directory_fact_t *fact, uint64_t now_us);
ucn_result_t ucn_i_cluster_directory_copy(
    ucn_i_cluster_owner_t *owner, uint32_t remote_cluster_id,
    uint64_t now_us, ucn_i_cluster_directory_fact_t *fact_out);
ucn_result_t ucn_i_cluster_tunnel_install(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_tunnel_request_t *request, uint64_t now_us);
ucn_result_t ucn_i_cluster_tunnel_preflight(
    ucn_i_cluster_owner_t *owner, uint64_t tunnel_id,
    const ucn_i_cluster_flow_fact_t *current_flow, uint64_t now_us,
    ucn_i_cluster_tunnel_request_t *tunnel_out);
ucn_result_t ucn_i_cluster_dependency_revoke(
    ucn_i_cluster_owner_t *owner,
    const uint8_t principal[UCN_I_CLUSTER_PRINCIPAL_BYTES],
    uint32_t binding_generation, uint32_t session_generation,
    uint32_t capability_generation, uint16_t link_id,
    uint32_t link_generation);

ucn_result_t ucn_i_cluster_bind_persistence(
    ucn_i_cluster_owner_t *owner, ucn_handle_t persistence_handle,
    const uint8_t published_body_digest[UCN_I_CLUSTER_DIGEST_BYTES]);
ucn_result_t ucn_i_cluster_accept_proof(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_proof_t *proof, uint64_t now_us);
ucn_result_t ucn_i_cluster_import(
    ucn_i_cluster_owner_t *owner, const uint8_t *body, size_t body_bytes,
    const ucn_i_cluster_durability_t *durability,
    const uint8_t published_body_digest[UCN_I_CLUSTER_DIGEST_BYTES]);
ucn_result_t ucn_i_cluster_step(
    ucn_i_cluster_owner_t *owner, uint64_t now_us, uint16_t budget,
    ucn_i_cluster_step_result_t *result_out);

/* Private cross-file helpers. */
bool ucn_i_cluster_p_owner_valid(const ucn_i_cluster_owner_t *owner);
bool ucn_i_cluster_p_config_valid(
    const ucn_i_cluster_config_view_t *config);
bool ucn_i_cluster_p_epoch_valid(const ucn_i_cluster_epoch_t *epoch);
bool ucn_i_cluster_p_principal_equal(const uint8_t *left,
                                     const uint8_t *right);
bool ucn_i_cluster_p_state_valid(const ucn_i_cluster_state_t *state);
ucn_result_t ucn_i_cluster_p_refresh(
    ucn_i_cluster_owner_t *owner, uint64_t now_us,
    uint32_t *live_old_out, uint32_t *live_new_out);
int ucn_i_cluster_p_config_find(
    const ucn_i_cluster_config_view_t *config,
    const uint8_t principal[UCN_I_CLUSTER_PRINCIPAL_BYTES]);
bool ucn_i_cluster_p_live_vote(
    const ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_config_view_t *config, uint8_t index,
    const ucn_i_cluster_vote_evidence_t *vote, uint64_t now_us);
bool ucn_i_cluster_p_quorum(
    const ucn_i_cluster_config_view_t *config, uint32_t mask);
bool ucn_i_cluster_p_snapshot_current(
    const ucn_i_cluster_owner_t *owner);
ucn_result_t ucn_i_cluster_p_prepare_requirement(
    ucn_i_cluster_owner_t *owner, uint16_t operation_kind,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out);
ucn_result_t ucn_i_cluster_p_record_encode(
    const ucn_i_cluster_state_t *state,
    uint8_t body_out[UCN_I_CLUSTER_RECORD_BYTES]);
ucn_result_t ucn_i_cluster_p_record_decode(
    const uint8_t *body, size_t body_bytes,
    ucn_i_cluster_state_t *state_out);

#endif
