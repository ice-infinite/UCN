#ifndef UCN_CLUSTER_WIRE_V4_SEMANTIC_H
#define UCN_CLUSTER_WIRE_V4_SEMANTIC_H

/* CLV2-05-03 private semantic layer for the frozen v4 raw codec.
 *
 * The public wire header deliberately exposes only the raw 40-byte frame.
 * This header is private to the Cluster codec and its focused tests.  It
 * gives every RFC4 Type an explicit payload owner so a caller cannot build a
 * message by carrying an unrelated six-word field bag between Types. */

#include "ucn/ucn_cluster_wire_v4.h"

typedef struct ucn_cluster_wire_v4_semantic_header {
    uint8_t type;
    ucn_cluster_role_t role;
    uint8_t flags;
    uint32_t cluster_id;
    uint32_t term;
    ucn_node_id_t head_node_id;
} ucn_cluster_wire_v4_semantic_header_t;

/* CLV2-05-07: RFC4 wire_offer remains one raw u32 on the wire, but private
 * codec users operate on explicit range/capability values. These constants
 * describe only frozen wire bits; they do not grant a Cluster role or make a
 * node eligible for Head/Backup selection. */
typedef enum ucn_cluster_wire_v4_capability {
    UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP = (uint16_t)0x0001U,
    UCN_CLUSTER_WIRE_V4_CAPABILITY_TAKEOVER = (uint16_t)0x0002U,
    UCN_CLUSTER_WIRE_V4_CAPABILITY_JOINT_CONFIG = (uint16_t)0x0004U,
    UCN_CLUSTER_WIRE_V4_CAPABILITY_PERSISTENCE = (uint16_t)0x0008U,
    UCN_CLUSTER_WIRE_V4_CAPABILITY_RECOVERY_LINEAGE = (uint16_t)0x0010U,
    UCN_CLUSTER_WIRE_V4_CAPABILITY_REKEY = (uint16_t)0x0020U
} ucn_cluster_wire_v4_capability_t;

#define UCN_CLUSTER_WIRE_V4_CAPABILITY_KNOWN_MASK ((uint16_t)0x003FU)

typedef struct ucn_cluster_wire_v4_wire_offer {
    uint8_t minimum_format;
    uint8_t maximum_format;
    uint16_t capabilities;
} ucn_cluster_wire_v4_wire_offer_t;

typedef struct ucn_cluster_wire_v4_selected_wire_offer {
    uint8_t format;
    uint16_t capabilities;
} ucn_cluster_wire_v4_selected_wire_offer_t;

/* CLV2-05-08: this is a private, stateless compatibility precondition, not
 * a Member/Config/Authority decision.  The caller says what class it is
 * considering and which RFC4 bits a future owning transaction requires;
 * this codec helper only enforces the mixed-v3/v4 boundary.  M06 owns real
 * member status, and M08/M10/M11/M13 own voter/Backup/Head authority. */
typedef enum ucn_cluster_wire_v4_mixed_version_policy {
    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_INVALID = 0,
    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4 = 1,
    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_ALLOW_V3_NON_VOTING_LEGACY = 2
} ucn_cluster_wire_v4_mixed_version_policy_t;

typedef enum ucn_cluster_wire_v4_peer_contract_class {
    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_INVALID = 0,
    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER = 1,
    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_VOTER = 2,
    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_BACKUP = 3,
    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_HEAD = 4
} ucn_cluster_wire_v4_peer_contract_class_t;

/* CLV2-05-11: isolated, caller-owned explanation of the 05-08 mixed-version
 * precondition.  This is intentionally not a Cluster Member/peer table and
 * must not be used as a Head/Backup/Voter authorization result.  A future RX
 * owner may create a view only after it has completed its own strict decode
 * and admission checks; this helper itself performs no I/O or state change. */
typedef enum ucn_cluster_wire_v4_peer_diagnostic_reason {
    UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_INVALID = 0,
    UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_COMPATIBLE_V4 = 1,
    UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_COMPATIBLE_V3_LEGACY = 2,
    UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_STRICT_V4_REQUIRES_V4 = 3,
    UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V3_LEGACY_NON_VOTING_ONLY = 4,
    UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V4_OFFER_MISSING = 5,
    UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V4_OFFER_INVALID = 6,
    UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V4_REQUIRED_CAPABILITY_MISSING = 7,
    /* A caller records this reason after diagnose_peer() reports
     * UCN_ERR_ARGUMENT.  No invalid input ever writes a diagnostic view. */
    UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_INPUT_INVALID = 8
} ucn_cluster_wire_v4_peer_diagnostic_reason_t;

typedef struct ucn_cluster_wire_v4_peer_diagnostic_input {
    ucn_node_id_t peer_node_id;
    ucn_cluster_wire_v4_mixed_version_policy_t policy;
    ucn_cluster_wire_format_t peer_format;
    bool peer_offer_present;
    ucn_cluster_wire_v4_wire_offer_t peer_offer;
    ucn_cluster_wire_v4_peer_contract_class_t requested_class;
    uint16_t required_v4_capabilities;
} ucn_cluster_wire_v4_peer_diagnostic_input_t;

typedef struct ucn_cluster_wire_v4_peer_diagnostic_view {
    ucn_node_id_t peer_node_id;
    ucn_cluster_wire_v4_mixed_version_policy_t policy;
    ucn_cluster_wire_format_t peer_format;
    bool peer_offer_present;
    ucn_cluster_wire_v4_wire_offer_t peer_offer;
    ucn_cluster_wire_v4_peer_contract_class_t requested_class;
    uint16_t required_v4_capabilities;
    /* Exactly UCN_OK or UCN_ERR_STATE.  It is a compatibility precondition,
     * never an actual role or Authority decision. */
    ucn_result_t compatibility;
    ucn_cluster_wire_v4_peer_diagnostic_reason_t reason;
} ucn_cluster_wire_v4_peer_diagnostic_view_t;

typedef struct ucn_cluster_wire_v4_peer_diagnostic_stats {
    uint32_t evaluated;
    uint32_t compatible_v4;
    uint32_t compatible_v3_legacy;
    uint32_t rejected;
    uint32_t invalid_inputs;
} ucn_cluster_wire_v4_peer_diagnostic_stats_t;

typedef char ucn_cluster_wire_v4_wire_offer_must_fit_raw_word[
    sizeof(ucn_cluster_wire_v4_wire_offer_t) <= sizeof(uint32_t) ? 1 : -1];
typedef char ucn_cluster_wire_v4_selected_wire_offer_must_fit_raw_word[
    sizeof(ucn_cluster_wire_v4_selected_wire_offer_t) <= sizeof(uint32_t) ?
        1 :
        -1];

typedef struct ucn_cluster_wire_v4_advertise_payload {
    uint32_t score_capacity;
    uint32_t lease_ms;
    uint32_t advertise_nonce;
    uint32_t wire_offer;
} ucn_cluster_wire_v4_advertise_payload_t;

typedef struct ucn_cluster_wire_v4_join_request_payload {
    uint32_t join_txid;
    uint32_t wire_offer;
    uint32_t current_config_id;
    uint32_t boot_incarnation;
    uint32_t score_capacity;
    uint32_t join_nonce;
} ucn_cluster_wire_v4_join_request_payload_t;

typedef struct ucn_cluster_wire_v4_join_accept_payload {
    uint32_t join_txid;
    uint32_t target_config_id;
    uint32_t lease_ms;
    uint32_t member_flags;
    uint32_t selected_wire_offer;
    uint32_t member_nonce;
} ucn_cluster_wire_v4_join_accept_payload_t;

typedef struct ucn_cluster_wire_v4_join_reject_payload {
    uint32_t join_nonce;
    uint32_t reason;
    uint32_t retry_after_ms;
} ucn_cluster_wire_v4_join_reject_payload_t;

typedef struct ucn_cluster_wire_v4_keepalive_payload {
    uint32_t lease_ms;
    uint32_t keepalive_nonce;
} ucn_cluster_wire_v4_keepalive_payload_t;

typedef struct ucn_cluster_wire_v4_leave_payload {
    uint32_t leave_nonce;
    uint32_t reason;
} ucn_cluster_wire_v4_leave_payload_t;

typedef struct ucn_cluster_wire_v4_head_declare_payload {
    uint32_t score_capacity;
    uint32_t lease_ms;
    uint32_t declare_nonce;
    uint32_t wire_offer;
} ucn_cluster_wire_v4_head_declare_payload_t;

typedef struct ucn_cluster_wire_v4_head_takeover_payload {
    uint32_t backup_generation;
    uint32_t snapshot_id;
    uint32_t certificate_anchor_config_id;
    uint32_t takeover_txid;
    uint32_t required_set_mask;
    uint32_t certificate_crc32;
} ucn_cluster_wire_v4_head_takeover_payload_t;

typedef struct ucn_cluster_wire_v4_head_stepdown_payload {
    uint32_t handover_txid;
    uint32_t target_cluster_id;
    uint32_t target_term;
    ucn_node_id_t target_head_node_id;
    uint32_t stepdown_nonce;
} ucn_cluster_wire_v4_head_stepdown_payload_t;

typedef struct ucn_cluster_wire_v4_backup_assign_payload {
    uint32_t backup_generation;
    ucn_node_id_t backup_node_id;
    uint32_t sync_token;
    uint32_t config_id;
    uint32_t config_hash;
} ucn_cluster_wire_v4_backup_assign_payload_t;

typedef struct ucn_cluster_wire_v4_backup_ready_payload {
    uint32_t backup_generation;
    uint32_t snapshot_id;
    uint32_t membership_sequence;
    uint32_t config_id;
    uint32_t config_hash;
    uint32_t ready_nonce;
} ucn_cluster_wire_v4_backup_ready_payload_t;

typedef struct ucn_cluster_wire_v4_backup_member_sync_payload {
    uint32_t backup_generation;
    uint32_t snapshot_id;
    uint32_t membership_sequence;
    ucn_node_id_t member_node_id;
    uint32_t member_nonce;
    uint32_t member_lease_ms;
} ucn_cluster_wire_v4_backup_member_sync_payload_t;

typedef struct ucn_cluster_wire_v4_primary_heartbeat_payload {
    uint32_t backup_generation;
    uint32_t config_id;
    uint32_t snapshot_id;
    uint32_t membership_sequence;
    uint32_t lease_ms;
    uint32_t heartbeat_nonce;
} ucn_cluster_wire_v4_primary_heartbeat_payload_t;

typedef struct ucn_cluster_wire_v4_takeover_vote_payload {
    uint32_t backup_generation;
    uint32_t snapshot_id;
    uint32_t config_id;
    uint32_t proposed_term;
    uint32_t takeover_txid;
    uint32_t nonce;
} ucn_cluster_wire_v4_takeover_vote_payload_t;

typedef struct ucn_cluster_wire_v4_recovery_declare_payload {
    uint32_t parent_cluster_id;
    uint32_t parent_term;
    uint32_t parent_config_id;
    uint32_t recovery_round;
    uint32_t recovery_nonce;
    uint32_t recovery_ttl_ms;
} ucn_cluster_wire_v4_recovery_declare_payload_t;

typedef struct ucn_cluster_wire_v4_recovery_ack_payload {
    uint32_t recovery_nonce;
    uint32_t member_nonce;
} ucn_cluster_wire_v4_recovery_ack_payload_t;

typedef struct ucn_cluster_wire_v4_backup_resync_payload {
    uint32_t backup_generation;
    uint32_t snapshot_id;
    uint32_t expected_membership_sequence;
    uint32_t request_nonce;
} ucn_cluster_wire_v4_backup_resync_payload_t;

typedef struct ucn_cluster_wire_v4_backup_reject_payload {
    uint32_t backup_generation;
    uint32_t config_id;
    uint32_t reason;
    uint32_t reject_nonce;
} ucn_cluster_wire_v4_backup_reject_payload_t;

typedef struct ucn_cluster_wire_v4_config_begin_payload {
    uint32_t config_txid;
    uint32_t old_config_id;
    uint32_t proposed_config_id;
    uint32_t old_config_hash;
    uint32_t proposed_config_hash;
    uint32_t proposal_nonce;
} ucn_cluster_wire_v4_config_begin_payload_t;

typedef struct ucn_cluster_wire_v4_config_member_payload {
    uint32_t config_txid;
    uint32_t target_config_id;
    ucn_node_id_t member_node_id;
    uint32_t member_nonce;
    uint32_t member_capabilities;
    uint32_t ordinal_count;
} ucn_cluster_wire_v4_config_member_payload_t;

typedef struct ucn_cluster_wire_v4_config_prepare_payload {
    uint32_t proposed_config_id;
    uint32_t old_config_hash;
    uint32_t proposed_config_hash;
    uint32_t old_new_voter_count;
    uint32_t config_txid;
    uint32_t prepare_nonce;
} ucn_cluster_wire_v4_config_prepare_payload_t;

typedef struct ucn_cluster_wire_v4_config_ack_payload {
    uint32_t proposed_config_id;
    uint32_t config_txid;
    uint32_t voter_slot;
    uint32_t config_phase;
    uint32_t persistence_generation;
    uint32_t ack_nonce;
} ucn_cluster_wire_v4_config_ack_payload_t;

typedef struct ucn_cluster_wire_v4_config_commit_payload {
    uint32_t committed_config_id;
    uint32_t config_txid;
    uint32_t committed_config_hash;
    uint32_t committed_voter_count;
    uint32_t commit_nonce;
} ucn_cluster_wire_v4_config_commit_payload_t;

typedef struct ucn_cluster_wire_v4_config_abort_payload {
    uint32_t config_txid;
    uint32_t old_config_id;
    uint32_t aborted_config_id;
    uint32_t reason;
    uint32_t abort_nonce;
} ucn_cluster_wire_v4_config_abort_payload_t;

typedef struct ucn_cluster_wire_v4_handover_payload {
    uint32_t handover_txid;
    uint32_t target_cluster_id;
    uint32_t target_term;
    ucn_node_id_t target_head_node_id;
    uint32_t target_config_id;
    uint32_t target_config_hash;
} ucn_cluster_wire_v4_handover_payload_t;

typedef struct ucn_cluster_wire_v4_head_withdraw_payload {
    uint32_t handover_txid;
    uint32_t target_cluster_id;
    uint32_t target_term;
    ucn_node_id_t target_head_node_id;
    uint32_t withdraw_nonce;
} ucn_cluster_wire_v4_head_withdraw_payload_t;

typedef struct ucn_cluster_wire_v4_rekey_payload {
    uint32_t successor_cluster_id;
    uint32_t successor_term;
    uint32_t rekey_txid;
    uint32_t old_config_id;
    uint32_t successor_config_id;
    uint32_t nonce;
} ucn_cluster_wire_v4_rekey_payload_t;

typedef struct ucn_cluster_wire_v4_rekey_ack_payload {
    uint32_t successor_cluster_id;
    uint32_t successor_term;
    uint32_t rekey_txid;
    uint32_t successor_config_id;
    uint32_t persistence_generation;
    uint32_t member_nonce;
} ucn_cluster_wire_v4_rekey_ack_payload_t;

typedef struct ucn_cluster_wire_v4_takeover_certificate_payload {
    uint32_t backup_generation;
    uint32_t snapshot_id;
    uint32_t config_id;
    uint32_t takeover_txid;
    uint32_t fragment_descriptor;
    uint32_t vote_bitmap_word;
} ucn_cluster_wire_v4_takeover_certificate_payload_t;

typedef union ucn_cluster_wire_v4_semantic_payload {
    ucn_cluster_wire_v4_advertise_payload_t advertise;
    ucn_cluster_wire_v4_join_request_payload_t join_request;
    ucn_cluster_wire_v4_join_accept_payload_t join_accept;
    ucn_cluster_wire_v4_join_reject_payload_t join_reject;
    ucn_cluster_wire_v4_keepalive_payload_t keepalive;
    ucn_cluster_wire_v4_leave_payload_t leave;
    ucn_cluster_wire_v4_head_declare_payload_t head_declare;
    ucn_cluster_wire_v4_head_takeover_payload_t head_takeover;
    ucn_cluster_wire_v4_head_stepdown_payload_t head_stepdown;
    ucn_cluster_wire_v4_backup_assign_payload_t backup_assign;
    ucn_cluster_wire_v4_backup_ready_payload_t backup_ready;
    ucn_cluster_wire_v4_backup_member_sync_payload_t backup_member_sync;
    ucn_cluster_wire_v4_primary_heartbeat_payload_t primary_heartbeat;
    ucn_cluster_wire_v4_takeover_vote_payload_t takeover_prepare;
    ucn_cluster_wire_v4_takeover_vote_payload_t takeover_ack;
    ucn_cluster_wire_v4_recovery_declare_payload_t recovery_declare;
    ucn_cluster_wire_v4_recovery_ack_payload_t recovery_ack;
    ucn_cluster_wire_v4_backup_resync_payload_t backup_resync;
    ucn_cluster_wire_v4_backup_reject_payload_t backup_reject;
    ucn_cluster_wire_v4_config_begin_payload_t config_begin;
    ucn_cluster_wire_v4_config_member_payload_t config_member;
    ucn_cluster_wire_v4_config_prepare_payload_t config_prepare;
    ucn_cluster_wire_v4_config_ack_payload_t config_ack;
    ucn_cluster_wire_v4_config_commit_payload_t config_commit;
    ucn_cluster_wire_v4_config_abort_payload_t config_abort;
    ucn_cluster_wire_v4_handover_payload_t handover_prepare;
    ucn_cluster_wire_v4_handover_payload_t handover_ready;
    ucn_cluster_wire_v4_handover_payload_t handover_commit;
    ucn_cluster_wire_v4_head_withdraw_payload_t head_withdraw;
    ucn_cluster_wire_v4_rekey_payload_t rekey_prepare;
    ucn_cluster_wire_v4_rekey_ack_payload_t rekey_ack;
    ucn_cluster_wire_v4_rekey_payload_t rekey_commit;
    ucn_cluster_wire_v4_takeover_certificate_payload_t takeover_certificate;
} ucn_cluster_wire_v4_semantic_payload_t;

/* The largest RFC4 payload is exactly six u32 values.  Keep this private
 * semantic representation bounded by the raw payload size; it is transient
 * caller stack data and is never embedded in ucn_cluster_t. */
typedef char ucn_cluster_wire_v4_semantic_payload_must_fit_raw_payload[
    sizeof(ucn_cluster_wire_v4_semantic_payload_t) <=
            UCN_CLUSTER_WIRE_V4_WORD_COUNT * sizeof(uint32_t) ?
        1 :
        -1];

typedef struct ucn_cluster_wire_v4_semantic_message {
    ucn_cluster_wire_v4_semantic_header_t header;
    ucn_cluster_wire_v4_semantic_payload_t payload;
} ucn_cluster_wire_v4_semantic_message_t;

/* CLV2-05-04: Type 12 is not merely a member record. Every snapshot item
 * is bound to its complete RFC4 Epoch and to generation/snapshot/sequence
 * before its marker or member fields are interpreted. This remains a
 * private, transient codec object: it is neither a receive state machine nor
 * storage inside ucn_cluster_t. */
typedef enum ucn_cluster_wire_v4_snapshot_kind {
    UCN_CLUSTER_WIRE_V4_SNAPSHOT_INVALID = 0,
    UCN_CLUSTER_WIRE_V4_SNAPSHOT_MEMBER = 1,
    UCN_CLUSTER_WIRE_V4_SNAPSHOT_BEGIN = 2,
    UCN_CLUSTER_WIRE_V4_SNAPSHOT_END = 3,
    UCN_CLUSTER_WIRE_V4_SNAPSHOT_DELTA = 4
} ucn_cluster_wire_v4_snapshot_kind_t;

typedef struct ucn_cluster_wire_v4_snapshot {
    uint32_t cluster_id;
    uint32_t term;
    ucn_node_id_t head_node_id;
    uint32_t backup_generation;
    uint32_t snapshot_id;
    uint32_t membership_sequence;
    ucn_cluster_wire_v4_snapshot_kind_t kind;
    ucn_node_id_t member_node_id;
    uint32_t member_nonce;
    uint32_t member_lease_ms;
} ucn_cluster_wire_v4_snapshot_t;

typedef char ucn_cluster_wire_v4_snapshot_must_not_exceed_raw_frame[
    sizeof(ucn_cluster_wire_v4_snapshot_t) <=
            UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES ?
        1 :
        -1];

/* CLV2-05-05: Type 8 and Type 33 together carry a Takeover Certificate.
 * These are transient private codec values, not a Certificate validator and
 * not state inside ucn_cluster_t.  Keeping the proposed Epoch in both
 * objects prevents a caller from binding a fragment to an unrelated new
 * Head by passing a loose P0..P5 bag around. */
typedef enum ucn_cluster_wire_v4_certificate_set {
    UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_INVALID = 0,
    UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_OLD = 1,
    UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_NEW = 2
} ucn_cluster_wire_v4_certificate_set_t;

typedef struct ucn_cluster_wire_v4_takeover {
    uint32_t cluster_id;
    uint32_t proposed_term;
    ucn_node_id_t proposed_head_node_id;
    uint32_t backup_generation;
    uint32_t snapshot_id;
    uint32_t certificate_anchor_config_id;
    uint32_t takeover_txid;
    uint32_t required_set_mask;
    uint32_t certificate_crc32;
} ucn_cluster_wire_v4_takeover_t;

typedef struct ucn_cluster_wire_v4_takeover_fragment {
    uint32_t cluster_id;
    uint32_t proposed_term;
    ucn_node_id_t proposed_head_node_id;
    uint32_t backup_generation;
    uint32_t snapshot_id;
    uint32_t config_id;
    uint32_t takeover_txid;
    ucn_cluster_wire_v4_certificate_set_t certificate_set;
    uint16_t fragment_index;
    uint16_t fragment_count;
    uint32_t vote_bitmap_word;
} ucn_cluster_wire_v4_takeover_fragment_t;

typedef char ucn_cluster_wire_v4_takeover_must_not_exceed_raw_frame[
    sizeof(ucn_cluster_wire_v4_takeover_t) <=
            UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES ?
        1 :
        -1];
typedef char ucn_cluster_wire_v4_takeover_fragment_must_not_exceed_raw_frame[
    sizeof(ucn_cluster_wire_v4_takeover_fragment_t) <=
            UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES ?
        1 :
        -1];

/* These builders are private codec helpers.  They never send a frame or
 * consult Cluster state.  On failure their output argument is unchanged. */
ucn_result_t ucn_cluster_wire_v4_semantic_from_frame(
    const ucn_cluster_wire_v4_frame_t *frame,
    ucn_cluster_wire_v4_semantic_message_t *output);
ucn_result_t ucn_cluster_wire_v4_semantic_to_frame(
    const ucn_cluster_wire_v4_semantic_message_t *message,
    ucn_cluster_wire_v4_frame_t *output);

/* Test-only introspection for proving that bytes beyond the active Type
 * payload are not read by semantic_to_frame().  Zero means an unknown Type. */
size_t ucn_cluster_wire_v4_semantic_payload_size(uint8_t type);

/* Type 12-only helpers. They retain the complete Epoch rather than exposing
 * a partial member-only view. On failure output remains unchanged. */
ucn_result_t ucn_cluster_wire_v4_snapshot_from_frame(
    const ucn_cluster_wire_v4_frame_t *frame,
    ucn_cluster_wire_v4_snapshot_t *output);
ucn_result_t ucn_cluster_wire_v4_snapshot_to_frame(
    const ucn_cluster_wire_v4_snapshot_t *snapshot,
    ucn_cluster_wire_v4_frame_t *output);

/* Type 8/33-only helpers.  Their failure paths leave output unchanged.
 * fragment_matches_admission() only checks RFC4 carrier/key/Config binding;
 * it intentionally does not calculate certificate CRC, voter ordering,
 * VoteId or quorum, which remain M10 responsibilities. */
ucn_result_t ucn_cluster_wire_v4_takeover_from_frame(
    const ucn_cluster_wire_v4_frame_t *frame,
    ucn_cluster_wire_v4_takeover_t *output);
ucn_result_t ucn_cluster_wire_v4_takeover_to_frame(
    const ucn_cluster_wire_v4_takeover_t *takeover,
    ucn_cluster_wire_v4_frame_t *output);
ucn_result_t ucn_cluster_wire_v4_takeover_fragment_from_frame(
    const ucn_cluster_wire_v4_frame_t *frame,
    ucn_cluster_wire_v4_takeover_fragment_t *output);
ucn_result_t ucn_cluster_wire_v4_takeover_fragment_to_frame(
    const ucn_cluster_wire_v4_takeover_fragment_t *fragment,
    ucn_cluster_wire_v4_frame_t *output);
bool ucn_cluster_wire_v4_takeover_fragment_matches_admission(
    const ucn_cluster_wire_v4_takeover_t *takeover,
    const ucn_cluster_wire_v4_takeover_fragment_t *fragment,
    const ucn_cluster_wire_v4_certificate_admission_t *admission);

/* RFC4 wire_offer helpers.  All conversions fail closed and leave output
 * unchanged. negotiate() is pure: it returns the highest common format and
 * the common capability set, but never modifies Cluster state or authorizes
 * Head/Backup eligibility. */
ucn_result_t ucn_cluster_wire_v4_wire_offer_from_word(
    uint32_t word,
    ucn_cluster_wire_v4_wire_offer_t *output);
ucn_result_t ucn_cluster_wire_v4_wire_offer_to_word(
    const ucn_cluster_wire_v4_wire_offer_t *offer,
    uint32_t *output);
ucn_result_t ucn_cluster_wire_v4_selected_wire_offer_from_word(
    uint32_t word,
    ucn_cluster_wire_v4_selected_wire_offer_t *output);
ucn_result_t ucn_cluster_wire_v4_selected_wire_offer_to_word(
    const ucn_cluster_wire_v4_selected_wire_offer_t *offer,
    uint32_t *output);
bool ucn_cluster_wire_v4_wire_offer_supports(
    const ucn_cluster_wire_v4_wire_offer_t *offer,
    uint16_t required_capabilities);
ucn_result_t ucn_cluster_wire_v4_wire_offer_negotiate(
    const ucn_cluster_wire_v4_wire_offer_t *local_offer,
    const ucn_cluster_wire_v4_wire_offer_t *peer_offer,
    uint16_t required_capabilities,
    ucn_cluster_wire_v4_selected_wire_offer_t *output);

/* Mixed-version precondition only.  A v3 peer supplies no v4 offer and can
 * pass only when explicit legacy mode requests NON_VOTING_MEMBER with zero
 * required bits.  A v4 peer must supply a valid RFC4 offer covering the
 * caller-provided required bits.  UCN_OK does not authorize an actual
 * member, voter, Backup, Head, takeover or Authority transition. */
ucn_result_t ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
    ucn_cluster_wire_v4_mixed_version_policy_t policy,
    ucn_cluster_wire_format_t peer_format,
    const ucn_cluster_wire_v4_wire_offer_t *peer_offer,
    ucn_cluster_wire_v4_peer_contract_class_t requested_class,
    uint16_t required_v4_capabilities);

/* Produces a deterministic diagnostic view for a syntactically valid caller
 * observation.  A compatibility rejection is reported in view->reason and
 * view->compatibility == UCN_ERR_STATE, while the function still returns
 * UCN_OK.  Bad input returns UCN_ERR_ARGUMENT and leaves output unchanged.
 * It only explains 05-08's pure compatibility precondition. */
ucn_result_t ucn_cluster_wire_v4_diagnose_peer(
    const ucn_cluster_wire_v4_peer_diagnostic_input_t *input,
    ucn_cluster_wire_v4_peer_diagnostic_view_t *output);

/* Caller-owned, saturating diagnostic counter update.  Pass the reason from
 * a successful diagnose_peer() call, or INPUT_INVALID after an argument
 * failure.  This never reads or changes Cluster runtime state. */
ucn_result_t ucn_cluster_wire_v4_peer_diagnostic_stats_record(
    ucn_cluster_wire_v4_peer_diagnostic_stats_t *stats,
    ucn_cluster_wire_v4_peer_diagnostic_reason_t reason);

#endif
