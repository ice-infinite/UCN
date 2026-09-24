#ifndef UCN_INTERNAL_FLOW_H
#define UCN_INTERNAL_FLOW_H

#include "internal/ucn_digest.h"
#include "internal/ucn_route.h"

#define UCN_I_FLOW_SCHEMA UINT16_C(1)
#define UCN_I_FLOW_LABEL_SETUP_BYTES 16U
#define UCN_I_FLOW_C2_PREFIX_BYTES 9U
#define UCN_I_FLOW_C3_PREFIX_BYTES 7U
#define UCN_I_FLOW_C4_PREFIX_BYTES 11U

#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_I_FLOW_CANDIDATE_COUNT 2U
#define UCN_I_FLOW_ACTIVATION_COUNT 2U
#define UCN_I_FLOW_ACTIVE_COUNT 2U
#define UCN_I_FLOW_RECEIPT_COUNT 2U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_I_FLOW_CANDIDATE_COUNT 8U
#define UCN_I_FLOW_ACTIVATION_COUNT 8U
#define UCN_I_FLOW_ACTIVE_COUNT 8U
#define UCN_I_FLOW_RECEIPT_COUNT 8U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define UCN_I_FLOW_CANDIDATE_COUNT 24U
#define UCN_I_FLOW_ACTIVATION_COUNT 24U
#define UCN_I_FLOW_ACTIVE_COUNT 32U
#define UCN_I_FLOW_RECEIPT_COUNT 32U
#else
#error "UCN_PROFILE must be UCN_PROFILE_NANO, UCN_PROFILE_LITE, or UCN_PROFILE_FULL"
#endif

typedef struct ucn_i_flow_common_header {
    uint8_t contract;
    uint8_t traffic_class;
    uint8_t delivery;
    uint8_t interaction;
    uint8_t payload_kind;
    uint8_t origin_security;
    uint8_t hop_limit;
    uint8_t reserved_zero;
} ucn_i_flow_common_header_t;

typedef struct ucn_i_flow_label_setup {
    uint32_t candidate_id;
    uint32_t route_generation;
    uint32_t context_digest;
    uint16_t reverse_label;
    uint16_t forward_label;
    uint16_t path_profile_id;
    uint16_t reserved_zero;
} ucn_i_flow_label_setup_t;

typedef struct ucn_i_flow_prefix {
    ucn_i_flow_common_header_t header;
    uint32_t sequence;
    uint16_t label;
    uint16_t context_id;
} ucn_i_flow_prefix_t;

typedef struct ucn_i_flow_capability_facts {
    uint8_t digest[16];
    uint64_t expires_at_us;
    uint32_t runtime_instance;
    uint32_t session_generation;
    uint32_t capability_generation;
    uint32_t feature_bits;
    uint16_t security_owner_instance;
    uint16_t reserved_zero;
} ucn_i_flow_capability_facts_t;

typedef struct ucn_i_flow_requirements {
    uint64_t expires_at_us;
    uint32_t required_feature_bits;
    uint32_t policy_generation;
    uint16_t service_id;
    uint16_t protocol_opcode;
    uint16_t minimum_payload_bytes;
    uint16_t path_profile_id;
    uint8_t contract;
    uint8_t traffic_ceiling;
    uint8_t delivery;
    uint8_t interaction;
    uint8_t payload_kind;
    uint8_t origin_security;
    uint8_t hop_profile;
    uint8_t reserved_zero;
} ucn_i_flow_requirements_t;

typedef uint8_t ucn_i_flow_phase_t;
enum {
    UCN_I_FLOW_CANDIDATE = 1,
    UCN_I_FLOW_PROBING = 2,
    UCN_I_FLOW_READY_TO_STAGE = 3,
    UCN_I_FLOW_STAGE_PENDING = 4,
    UCN_I_FLOW_STAGED = 5,
    UCN_I_FLOW_COMMITTING = 6,
    UCN_I_FLOW_ACTIVE = 7,
    UCN_I_FLOW_IN_DOUBT = 8,
    UCN_I_FLOW_ABORTING = 9,
    UCN_I_FLOW_DRAINING = 10,
    UCN_I_FLOW_FENCED = 11,
    UCN_I_FLOW_RETIRED = 12
};

typedef uint8_t ucn_i_flow_submit_result_t;
enum {
    UCN_I_FLOW_NOT_SUBMITTED = 1,
    UCN_I_FLOW_SUBMITTED = 2,
    UCN_I_FLOW_SUBMIT_IN_DOUBT = 3
};

typedef struct ucn_i_flow_activation_key {
    ucn_i_route_domain_t route_domain;
    uint8_t proposal_digest[16];
    uint64_t transaction_id;
    uint32_t candidate_id;
    uint32_t route_generation;
    uint16_t reverse_label;
    uint16_t forward_label;
    uint16_t path_profile_id;
    uint16_t reserved_zero;
} ucn_i_flow_activation_key_t;

typedef struct ucn_i_flow_proposal {
    ucn_i_flow_activation_key_t key;
    ucn_i_route_soft_view_t source_route;
    ucn_i_flow_capability_facts_t capability;
    ucn_i_flow_requirements_t requirements;
    uint64_t probe_deadline_us;
    uint64_t flow_expires_at_us;
    uint32_t context_id;
    uint16_t payload_budget;
    uint16_t reserved_zero;
} ucn_i_flow_proposal_t;

typedef struct ucn_i_flow_probe_ack {
    uint8_t proposal_digest[16];
    uint32_t candidate_id;
    uint32_t route_generation;
    uint32_t measured_rtt_us;
    uint16_t path_frame_mtu;
    uint16_t capability_bits;
} ucn_i_flow_probe_ack_t;

typedef struct ucn_i_flow_current_facts {
    ucn_i_route_binding_t local;
    ucn_i_route_binding_t destination;
    ucn_i_route_link_ref_t next_hop;
    uint8_t capability_digest[16];
    uint64_t now_us;
    uint64_t route_causal_id;
    uint64_t route_deadline_us;
    uint64_t capability_deadline_us;
    uint32_t route_runtime_instance;
    uint32_t route_generation;
    uint32_t origin_session_generation;
    uint32_t capability_session_generation;
    uint32_t capability_generation;
    uint32_t policy_generation;
    uint16_t route_owner_instance;
    uint16_t capability_security_owner_instance;
    uint16_t reserved_zero[2];
} ucn_i_flow_current_facts_t;

typedef struct ucn_i_flow_tx_request {
    uint16_t protocol_opcode;
    uint16_t payload_bytes;
    uint8_t contract;
    uint8_t traffic_class;
    uint8_t delivery;
    uint8_t interaction;
    uint8_t payload_kind;
    uint8_t origin_security;
    uint8_t reserved_zero[2];
} ucn_i_flow_tx_request_t;

typedef struct ucn_i_flow_tx_plan {
    ucn_i_route_link_ref_t next_hop;
    uint8_t flow_fingerprint[16];
    uint32_t route_generation;
    uint32_t sequence;
    uint16_t context_id;
    uint16_t forward_label;
    uint16_t payload_budget;
    uint8_t contract;
    uint8_t hop_profile;
} ucn_i_flow_tx_plan_t;

typedef struct ucn_i_flow_config {
    ucn_i_route_binding_t local;
    uint64_t probe_lifetime_us;
    uint64_t stage_lifetime_us;
    uint64_t commit_lifetime_us;
    uint64_t flow_lifetime_us;
    uint64_t receipt_lifetime_us;
    uint64_t first_transaction_id;
    uint32_t runtime_instance;
    uint32_t realm;
    uint32_t policy_generation;
    uint32_t first_candidate_id;
    uint32_t first_route_generation;
    uint32_t first_flow_generation;
    uint16_t owner_instance;
    uint16_t first_context_id;
    uint16_t first_label;
    uint16_t reserved_zero;
} ucn_i_flow_config_t;

typedef struct ucn_i_flow_candidate_record {
    ucn_i_flow_proposal_t proposal;
    uint16_t generation;
    uint8_t phase;
    uint8_t occupied;
} ucn_i_flow_candidate_record_t;

typedef struct ucn_i_flow_activation_record {
    ucn_i_flow_proposal_t proposal;
    uint64_t stage_deadline_us;
    uint64_t commit_deadline_us;
    uint16_t generation;
    uint16_t flow_slot;
    uint16_t receipt_slot;
    uint8_t phase;
    uint8_t occupied;
    uint8_t stage_submitted;
    uint8_t commit_submitted;
} ucn_i_flow_activation_record_t;

typedef struct ucn_i_flow_active_record {
    ucn_i_flow_proposal_t proposal;
    uint8_t fingerprint[16];
    uint32_t flow_generation;
    uint32_t next_sequence;
    uint32_t reserved_sequence;
    uint16_t generation;
    uint8_t phase;
    uint8_t occupied;
    uint8_t sequence_reserved;
    uint8_t reserved_zero;
} ucn_i_flow_active_record_t;

typedef struct ucn_i_flow_receipt_record {
    ucn_i_flow_activation_key_t key;
    uint64_t expires_at_us;
    uint8_t phase;
    uint8_t occupied;
    uint8_t reserved_zero[6];
} ucn_i_flow_receipt_record_t;

typedef struct ucn_i_flow_owner {
    uint32_t magic;
    uint32_t runtime_instance;
    uint32_t realm;
    uint32_t policy_generation;
    uint64_t probe_lifetime_us;
    uint64_t stage_lifetime_us;
    uint64_t commit_lifetime_us;
    uint64_t flow_lifetime_us;
    uint64_t receipt_lifetime_us;
    uint64_t next_transaction_id;
    ucn_i_route_binding_t local;
    ucn_i_lock_ops_t state_lock;
    ucn_i_sha256_workspace_t digest_workspace;
    uint32_t next_candidate_id;
    uint32_t next_route_generation;
    uint32_t next_flow_generation;
    uint16_t next_context_id;
    uint16_t next_label;
    uint16_t schema;
    uint16_t owner_instance;
    uint16_t maintenance_cursor;
    uint16_t reserved_zero;
    ucn_i_flow_candidate_record_t candidates[UCN_I_FLOW_CANDIDATE_COUNT];
    ucn_i_flow_activation_record_t activations[UCN_I_FLOW_ACTIVATION_COUNT];
    ucn_i_flow_active_record_t flows[UCN_I_FLOW_ACTIVE_COUNT];
    ucn_i_flow_receipt_record_t receipts[UCN_I_FLOW_RECEIPT_COUNT];
} ucn_i_flow_owner_t;

ucn_result_t ucn_i_flow_label_setup_encode(
    const ucn_i_flow_label_setup_t *value,
    uint8_t output[UCN_I_FLOW_LABEL_SETUP_BYTES]);
ucn_result_t ucn_i_flow_label_setup_decode(
    const uint8_t input[UCN_I_FLOW_LABEL_SETUP_BYTES],
    ucn_i_flow_label_setup_t *value_out);
ucn_result_t ucn_i_flow_prefix_encode(const ucn_i_flow_prefix_t *value,
                                      uint8_t *output,
                                      size_t output_capacity,
                                      size_t *output_bytes);
ucn_result_t ucn_i_flow_prefix_decode(const uint8_t *input,
                                      size_t input_bytes,
                                      ucn_i_flow_prefix_t *value_out);

ucn_result_t ucn_i_flow_owner_init(ucn_i_flow_owner_t *owner,
                                   const ucn_i_flow_config_t *config,
                                   const ucn_i_lock_ops_t *state_lock);
ucn_result_t ucn_i_flow_owner_destroy(ucn_i_flow_owner_t *owner);
ucn_result_t ucn_i_flow_import_candidate(
    ucn_i_flow_owner_t *owner,
    const ucn_i_route_soft_view_t *route,
    const ucn_i_flow_capability_facts_t *capability,
    const ucn_i_flow_requirements_t *requirements,
    uint64_t now_us,
    ucn_handle_t *candidate_out);
ucn_result_t ucn_i_flow_begin_probe(ucn_i_flow_owner_t *owner,
                                    ucn_handle_t candidate,
                                    uint64_t now_us,
                                    ucn_i_flow_proposal_t *proposal_out);
ucn_result_t ucn_i_flow_accept_probe_ack(
    ucn_i_flow_owner_t *owner,
    ucn_handle_t candidate,
    const ucn_i_flow_probe_ack_t *ack,
    uint64_t now_us);
ucn_result_t ucn_i_flow_begin_stage(ucn_i_flow_owner_t *owner,
                                    ucn_handle_t candidate,
                                    const ucn_i_flow_current_facts_t *facts,
                                    ucn_handle_t *activation_out,
                                    ucn_i_flow_label_setup_t *setup_out);
ucn_result_t ucn_i_flow_stage_submit(ucn_i_flow_owner_t *owner,
                                     ucn_handle_t activation,
                                     ucn_i_flow_submit_result_t result,
                                     uint64_t now_us);
ucn_result_t ucn_i_flow_accept_stage_ack(
    ucn_i_flow_owner_t *owner,
    ucn_handle_t activation,
    const ucn_i_flow_activation_key_t *key,
    const ucn_i_flow_current_facts_t *facts);
ucn_result_t ucn_i_flow_begin_commit(
    ucn_i_flow_owner_t *owner,
    ucn_handle_t activation,
    const ucn_i_flow_current_facts_t *facts,
    ucn_i_flow_activation_key_t *key_out);
ucn_result_t ucn_i_flow_commit_submit(ucn_i_flow_owner_t *owner,
                                      ucn_handle_t activation,
                                      ucn_i_flow_submit_result_t result,
                                      uint64_t now_us);
ucn_result_t ucn_i_flow_accept_commit_ack(
    ucn_i_flow_owner_t *owner,
    ucn_handle_t activation,
    const ucn_i_flow_activation_key_t *key,
    const ucn_i_flow_current_facts_t *facts,
    ucn_handle_t *flow_out);
ucn_result_t ucn_i_flow_expire_activation(ucn_i_flow_owner_t *owner,
                                          ucn_handle_t activation,
                                          uint64_t now_us,
                                          ucn_i_flow_phase_t *phase_out);
ucn_result_t ucn_i_flow_use_preflight(
    ucn_i_flow_owner_t *owner,
    ucn_handle_t flow,
    const ucn_i_flow_current_facts_t *facts,
    const ucn_i_flow_tx_request_t *request,
    ucn_i_flow_tx_plan_t *plan_out);
ucn_result_t ucn_i_flow_sequence_commit(ucn_i_flow_owner_t *owner,
                                        ucn_handle_t flow,
                                        uint32_t sequence);
ucn_result_t ucn_i_flow_sequence_abort(ucn_i_flow_owner_t *owner,
                                       ucn_handle_t flow,
                                       uint32_t sequence);
ucn_result_t ucn_i_flow_fence(ucn_i_flow_owner_t *owner,
                              ucn_handle_t flow);
ucn_result_t ucn_i_flow_phase(ucn_i_flow_owner_t *owner,
                              ucn_handle_t handle,
                              ucn_i_flow_phase_t *phase_out);
ucn_result_t ucn_i_flow_maintain(ucn_i_flow_owner_t *owner,
                                 uint64_t now_us,
                                 uint16_t budget,
                                 uint16_t *inspected_out,
                                 uint16_t *retired_out);
ucn_result_t ucn_i_flow_counts(ucn_i_flow_owner_t *owner,
                               uint16_t *candidates_out,
                               uint16_t *activations_out,
                               uint16_t *flows_out,
                               uint16_t *receipts_out);

#endif
