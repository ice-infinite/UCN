#ifndef UCN_INTERNAL_TRANSPORT_H
#define UCN_INTERNAL_TRANSPORT_H

#include "internal/ucn_digest.h"
#include "internal/ucn_owner.h"
#include "ucn/ucn_config.h"
#include "ucn/ucn_core.h"

#define UCN_I_TRANSPORT_SCHEMA UINT16_C(1)
#define UCN_I_TRANSPORT_PRINCIPAL_BYTES 16U
#define UCN_I_TRANSPORT_DIGEST_BYTES 16U
#define UCN_I_TRANSPORT_DELIVERY_ACK_BYTES 9U
#define UCN_I_TRANSPORT_TRANSFER_SETUP_ONE_WAY_BYTES 43U
#define UCN_I_TRANSPORT_TRANSFER_SETUP_OPERATION_BYTES 52U
#define UCN_I_TRANSPORT_TRANSFER_SETUP_RESULT_BYTES 54U
#define UCN_I_TRANSPORT_C1_FRAGMENT_PREFIX_BYTES 12U
#define UCN_I_TRANSPORT_FLOW_FRAGMENT_PREFIX_BYTES 8U
#define UCN_I_TRANSPORT_C1_SACK_BYTES 20U
#define UCN_I_TRANSPORT_FLOW_SACK_BYTES 14U
#define UCN_I_TRANSPORT_PARENT_RECORD_BYTES 88U
#define UCN_I_TRANSPORT_PARENT_SCHEMA_ID UINT16_C(0x5401)
#define UCN_I_TRANSPORT_PARENT_OPERATION_KIND UINT16_C(0x0701)

#define UCN_I_TRANSPORT_OPCODE_DELIVERY_ACK UINT16_C(0x0401)
#define UCN_I_TRANSPORT_OPCODE_PARENT_PREPARE UINT16_C(0x0410)
#define UCN_I_TRANSPORT_OPCODE_PARENT_ACCEPT UINT16_C(0x0411)
#define UCN_I_TRANSPORT_OPCODE_PARENT_COMMIT UINT16_C(0x0412)
#define UCN_I_TRANSPORT_OPCODE_PARENT_ABORT UINT16_C(0x0413)
#define UCN_I_TRANSPORT_OPCODE_TRANSFER_SETUP UINT16_C(0x0420)
#define UCN_I_TRANSPORT_OPCODE_TRANSFER_SETUP_ACK UINT16_C(0x0421)
#define UCN_I_TRANSPORT_OPCODE_TRANSFER_ABORT UINT16_C(0x0422)
#define UCN_I_TRANSPORT_OPCODE_TRANSFER_TERMINAL_RECEIPT UINT16_C(0x0423)
#define UCN_I_TRANSPORT_OPCODE_TRANSFER_SACK_CREDIT UINT16_C(0x0424)

#define UCN_I_TRANSPORT_ACK_SUBTYPE UINT8_C(1)
#define UCN_I_TRANSPORT_PARENT_KIND_C1 UINT8_C(1)
#define UCN_I_TRANSPORT_PARENT_KIND_FLOW UINT8_C(2)
#define UCN_I_TRANSPORT_FRAGMENT_KIND_MASK UINT16_C(0x8000)
#define UCN_I_TRANSPORT_SACK_KIND UINT16_C(0x8000)
#define UCN_I_TRANSPORT_MAX_FRAGMENT_COUNT UINT16_C(128)
#define UCN_I_TRANSPORT_FRAGMENT_BITMAP_BYTES 16U

#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_I_TRANSPORT_RELIABLE_TX_COUNT 2U
#define UCN_I_TRANSPORT_RELIABLE_RX_COUNT 2U
#define UCN_I_TRANSPORT_RELIABLE_BYTES 128U
#define UCN_I_TRANSPORT_TRANSFER_TX_COUNT 1U
#define UCN_I_TRANSPORT_TRANSFER_RX_COUNT 1U
#define UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT 1U
#define UCN_I_TRANSPORT_TRANSFER_BYTES 512U
#define UCN_I_TRANSPORT_TRANSFER_FRAGMENTS 16U
#define UCN_I_TRANSPORT_PARENT_COUNT 1U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_I_TRANSPORT_RELIABLE_TX_COUNT 4U
#define UCN_I_TRANSPORT_RELIABLE_RX_COUNT 8U
#define UCN_I_TRANSPORT_RELIABLE_BYTES 256U
#define UCN_I_TRANSPORT_TRANSFER_TX_COUNT 2U
#define UCN_I_TRANSPORT_TRANSFER_RX_COUNT 2U
#define UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT 2U
#define UCN_I_TRANSPORT_TRANSFER_BYTES 2048U
#define UCN_I_TRANSPORT_TRANSFER_FRAGMENTS 64U
#define UCN_I_TRANSPORT_PARENT_COUNT 2U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define UCN_I_TRANSPORT_RELIABLE_TX_COUNT 8U
#define UCN_I_TRANSPORT_RELIABLE_RX_COUNT 24U
#define UCN_I_TRANSPORT_RELIABLE_BYTES 512U
#define UCN_I_TRANSPORT_TRANSFER_TX_COUNT 4U
#define UCN_I_TRANSPORT_TRANSFER_RX_COUNT 4U
#define UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT 4U
#define UCN_I_TRANSPORT_TRANSFER_BYTES 4096U
#define UCN_I_TRANSPORT_TRANSFER_FRAGMENTS 128U
#define UCN_I_TRANSPORT_PARENT_COUNT 4U
#else
#error "UCN_PROFILE must be UCN_PROFILE_NANO, UCN_PROFILE_LITE, or UCN_PROFILE_FULL"
#endif

UCN_STATIC_ASSERT(UCN_I_TRANSPORT_TRANSFER_FRAGMENTS <=
                      UCN_I_TRANSPORT_MAX_FRAGMENT_COUNT,
                  transfer_fragment_capacity_exceeds_wire_limit);

typedef struct ucn_i_transport_binding {
    uint32_t address;
    uint32_t generation;
    uint8_t principal[UCN_I_TRANSPORT_PRINCIPAL_BYTES];
} ucn_i_transport_binding_t;

typedef struct ucn_i_transport_reliable_key {
    ucn_i_transport_binding_t source;
    ucn_i_transport_binding_t destination;
    uint32_t realm;
    uint32_t origin_sequence;
    uint16_t service_id;
    uint8_t delivery;
    uint8_t reserved_zero;
} ucn_i_transport_reliable_key_t;

typedef struct ucn_i_transport_security_facts {
    uint32_t session_generation;
    uint32_t key_generation;
    uint32_t link_generation;
    uint32_t policy_generation;
    uint16_t link_id;
    uint8_t origin_security;
    uint8_t endpoint_public_unauthenticated;
    uint8_t trusted_link_policy;
    uint8_t authenticated_replay_candidate;
} ucn_i_transport_security_facts_t;

typedef struct ucn_i_transport_delivery_ack {
    uint16_t service_id;
    uint32_t origin_sequence;
    uint8_t status;
    uint8_t receive_credit;
} ucn_i_transport_delivery_ack_t;

typedef struct ucn_i_transport_ack_facts {
    ucn_i_transport_binding_t source;
    ucn_i_transport_binding_t destination;
    ucn_i_transport_security_facts_t security;
    uint32_t realm;
    uint8_t reserved_zero[4];
} ucn_i_transport_ack_facts_t;

typedef uint8_t ucn_i_transport_reliable_phase_t;
enum {
    UCN_I_TRANSPORT_RELIABLE_READY = 1,
    UCN_I_TRANSPORT_RELIABLE_WAIT_ACK = 2,
    UCN_I_TRANSPORT_RELIABLE_DONE = 3,
    UCN_I_TRANSPORT_RELIABLE_FAILED = 4,
    UCN_I_TRANSPORT_RELIABLE_CANCELLED = 5
};

typedef struct ucn_i_transport_reliable_view {
    ucn_i_transport_reliable_key_t key;
    uint64_t deadline_us;
    uint64_t next_retry_us;
    ucn_result_t terminal_result;
    uint16_t sealed_bytes;
    uint8_t attempts;
    uint8_t phase;
} ucn_i_transport_reliable_view_t;

typedef struct ucn_i_transport_receive_action {
    ucn_i_transport_delivery_ack_t ack;
    uint8_t deliver_to_application;
    uint8_t exact_duplicate;
    uint8_t reserved_zero[6];
} ucn_i_transport_receive_action_t;

typedef struct ucn_i_transport_transfer_setup {
    uint8_t message_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    uint64_t operation_id;
    uint32_t parent_generation;
    uint32_t transfer_id;
    uint32_t total_length;
    uint32_t lifetime_ms;
    uint16_t parent_id;
    uint16_t service_id;
    uint16_t fragment_count;
    uint16_t fragment_budget;
    uint16_t result_code;
    uint8_t parent_kind;
    uint8_t delivery;
    uint8_t interaction;
    uint8_t operation_flags;
} ucn_i_transport_transfer_setup_t;

typedef struct ucn_i_transport_transfer_facts {
    ucn_i_transport_binding_t source;
    ucn_i_transport_binding_t destination;
    ucn_i_transport_security_facts_t security;
    uint8_t parent_fingerprint[UCN_I_TRANSPORT_DIGEST_BYTES];
    uint32_t realm;
    uint32_t transport_policy_generation;
    uint8_t parent_kind;
    uint8_t reserved_zero[3];
} ucn_i_transport_transfer_facts_t;

typedef struct ucn_i_transport_fragment_prefix {
    uint32_t transfer_id;
    uint32_t parent_generation;
    uint16_t fragment_index;
    uint16_t fragment_count;
    uint8_t parent_kind;
    uint8_t reserved_zero[3];
} ucn_i_transport_fragment_prefix_t;

typedef struct ucn_i_transport_sack {
    uint32_t transfer_id;
    uint32_t parent_generation;
    uint32_t bitmap;
    uint16_t original_service_id;
    uint16_t window_base;
    uint16_t receive_credit;
    uint8_t parent_kind;
    uint8_t reserved_zero[3];
} ucn_i_transport_sack_t;

typedef uint8_t ucn_i_transport_transfer_phase_t;
enum {
    UCN_I_TRANSPORT_TRANSFER_SETUP_PENDING = 1,
    UCN_I_TRANSPORT_TRANSFER_SENDING = 2,
    UCN_I_TRANSPORT_TRANSFER_RECEIVING = 3,
    UCN_I_TRANSPORT_TRANSFER_COMPLETE = 4,
    UCN_I_TRANSPORT_TRANSFER_DELIVERED = 5,
    UCN_I_TRANSPORT_TRANSFER_ABORTED = 6,
    UCN_I_TRANSPORT_TRANSFER_FAILED = 7
};

typedef struct ucn_i_transport_transfer_view {
    ucn_i_transport_transfer_setup_t setup;
    uint64_t deadline_us;
    uint32_t completed_bytes;
    uint16_t completed_fragments;
    uint8_t phase;
    uint8_t reserved_zero;
} ucn_i_transport_transfer_view_t;

typedef struct ucn_i_transport_terminal_receipt_view {
    ucn_i_transport_transfer_setup_t setup;
    uint64_t expires_at_us;
    uint8_t terminal_status;
    uint8_t reserved_zero[7];
} ucn_i_transport_terminal_receipt_view_t;

typedef struct ucn_i_transport_parent_context {
    ucn_i_transport_binding_t source;
    ucn_i_transport_binding_t destination;
    uint32_t realm;
    uint32_t transport_policy_generation;
    uint32_t security_session_generation;
    uint32_t security_key_generation;
    uint32_t parent_generation;
    uint8_t origin_security;
    uint8_t reserved_zero[3];
} ucn_i_transport_parent_context_t;

typedef struct ucn_i_transport_parent_durability_base {
    uint64_t domain_id;
    uint64_t prior_foundation_transaction_id;
    uint64_t next_foundation_transaction_id;
    uint64_t expected_record_generation;
    uint64_t absolute_deadline_us;
    uint32_t prior_transfer_high_water;
    uint16_t domain_generation;
    uint16_t reserved_zero;
    uint8_t expected_body_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    ucn_handle_t volatile_continuation;
} ucn_i_transport_parent_durability_base_t;

typedef struct ucn_i_transport_parent_requirement {
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
    uint16_t reserved_zero;
    uint8_t expected_body_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
} ucn_i_transport_parent_requirement_t;

typedef struct ucn_i_transport_parent_proof {
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
    uint8_t body_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
} ucn_i_transport_parent_proof_t;

typedef struct ucn_i_transport_parent_failure {
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
    uint8_t request_terminal;
    uint8_t reserved_zero[3];
} ucn_i_transport_parent_failure_t;

typedef struct ucn_i_transport_parent_view {
    ucn_i_transport_parent_context_t context;
    uint64_t domain_id;
    uint64_t record_generation;
    uint64_t foundation_transaction_id;
    uint32_t transfer_high_water;
    uint32_t granted_transfer_id;
    uint16_t domain_generation;
    uint8_t active;
    uint8_t grant_available;
    uint8_t reserved_zero[4];
} ucn_i_transport_parent_view_t;

typedef struct ucn_i_transport_config {
    uint64_t reliable_lifetime_us;
    uint64_t reliable_retry_us;
    uint64_t receipt_lifetime_us;
    uint32_t runtime_instance;
    uint16_t owner_instance;
    uint8_t reliable_max_attempts;
    uint8_t reserved_zero;
    ucn_i_lock_ops_t state_lock;
} ucn_i_transport_config_t;

typedef struct ucn_i_transport_reliable_tx_record {
    ucn_i_transport_reliable_key_t key;
    ucn_i_transport_security_facts_t security;
    uint8_t aad_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    uint8_t payload_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    uint8_t sealed[UCN_I_TRANSPORT_RELIABLE_BYTES];
    uint64_t deadline_us;
    uint64_t next_retry_us;
    ucn_result_t terminal_result;
    uint16_t sealed_bytes;
    uint16_t generation;
    uint8_t attempts;
    uint8_t phase;
    uint8_t occupied;
    uint8_t reserved_zero;
} ucn_i_transport_reliable_tx_record_t;

typedef struct ucn_i_transport_reliable_rx_record {
    ucn_i_transport_reliable_key_t key;
    ucn_i_transport_security_facts_t security;
    uint8_t aad_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    uint8_t payload_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    ucn_i_transport_delivery_ack_t ack;
    uint64_t expires_at_us;
    uint16_t generation;
    uint8_t occupied;
    uint8_t reserved_zero;
} ucn_i_transport_reliable_rx_record_t;

typedef struct ucn_i_transport_transfer_tx_record {
    ucn_i_transport_transfer_setup_t setup;
    ucn_i_transport_transfer_facts_t facts;
    uint8_t message[UCN_I_TRANSPORT_TRANSFER_BYTES];
    uint8_t acknowledged[UCN_I_TRANSPORT_FRAGMENT_BITMAP_BYTES];
    uint64_t deadline_us;
    uint32_t message_bytes;
    uint16_t generation;
    uint16_t acknowledged_count;
    uint8_t phase;
    uint8_t occupied;
    uint8_t setup_accepted;
    uint8_t reserved_zero;
} ucn_i_transport_transfer_tx_record_t;

typedef struct ucn_i_transport_transfer_rx_record {
    ucn_i_transport_transfer_setup_t setup;
    ucn_i_transport_transfer_facts_t facts;
    uint8_t message[UCN_I_TRANSPORT_TRANSFER_BYTES];
    uint8_t received[UCN_I_TRANSPORT_FRAGMENT_BITMAP_BYTES];
    uint8_t fragment_digest[UCN_I_TRANSPORT_TRANSFER_FRAGMENTS]
                           [UCN_I_TRANSPORT_DIGEST_BYTES];
    uint64_t deadline_us;
    uint32_t received_bytes;
    uint16_t generation;
    uint16_t received_count;
    uint16_t receipt_slot;
    uint8_t phase;
    uint8_t occupied;
    uint8_t copy_completed;
    uint8_t reserved_zero;
} ucn_i_transport_transfer_rx_record_t;

typedef struct ucn_i_transport_terminal_receipt_record {
    ucn_i_transport_transfer_setup_t setup;
    ucn_i_transport_transfer_facts_t facts;
    uint64_t expires_at_us;
    uint16_t generation;
    uint8_t occupied;
    uint8_t terminal;
    uint8_t terminal_status;
    uint8_t reserved_zero[3];
} ucn_i_transport_terminal_receipt_record_t;

typedef struct ucn_i_transport_parent_record {
    ucn_i_transport_parent_context_t context;
    ucn_i_transport_parent_durability_base_t durability;
    ucn_handle_t persistence_handle;
    uint8_t body[UCN_I_TRANSPORT_PARENT_RECORD_BYTES];
    uint8_t current_published_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    uint8_t pending_published_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    uint8_t canonical_body_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    uint64_t domain_id;
    uint64_t record_generation;
    uint64_t foundation_transaction_id;
    uint64_t transition_fingerprint;
    uint32_t transfer_high_water;
    uint32_t granted_transfer_id;
    uint16_t domain_generation;
    uint16_t generation;
    uint8_t occupied;
    uint8_t active;
    uint8_t prepared;
    uint8_t persistence_bound;
    uint8_t grant_available;
    uint8_t reserved_zero;
} ucn_i_transport_parent_record_t;

typedef struct ucn_i_transport_owner {
    uint32_t magic;
    uint32_t runtime_instance;
    uint64_t reliable_lifetime_us;
    uint64_t reliable_retry_us;
    uint64_t receipt_lifetime_us;
    uint16_t schema;
    uint16_t owner_instance;
    uint16_t maintenance_cursor;
    uint8_t reliable_max_attempts;
    uint8_t reserved_zero;
    ucn_i_lock_ops_t state_lock;
    ucn_i_sha256_workspace_t hash_workspace;
    ucn_i_transport_reliable_tx_record_t reliable_tx[
        UCN_I_TRANSPORT_RELIABLE_TX_COUNT];
    ucn_i_transport_reliable_rx_record_t reliable_rx[
        UCN_I_TRANSPORT_RELIABLE_RX_COUNT];
    ucn_i_transport_transfer_tx_record_t transfer_tx[
        UCN_I_TRANSPORT_TRANSFER_TX_COUNT];
    ucn_i_transport_transfer_rx_record_t transfer_rx[
        UCN_I_TRANSPORT_TRANSFER_RX_COUNT];
    ucn_i_transport_terminal_receipt_record_t transfer_receipts[
        UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT];
    ucn_i_transport_parent_record_t parents[UCN_I_TRANSPORT_PARENT_COUNT];
} ucn_i_transport_owner_t;

ucn_result_t ucn_i_transport_delivery_ack_encode(
    const ucn_i_transport_delivery_ack_t *value,
    uint8_t output[UCN_I_TRANSPORT_DELIVERY_ACK_BYTES]);
ucn_result_t ucn_i_transport_delivery_ack_decode(
    const uint8_t input[UCN_I_TRANSPORT_DELIVERY_ACK_BYTES],
    ucn_i_transport_delivery_ack_t *value_out);
ucn_result_t ucn_i_transport_transfer_setup_encode(
    const ucn_i_transport_transfer_setup_t *value,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes);
ucn_result_t ucn_i_transport_transfer_setup_decode(
    const uint8_t *input,
    size_t input_bytes,
    ucn_i_transport_transfer_setup_t *value_out);
ucn_result_t ucn_i_transport_fragment_prefix_encode(
    const ucn_i_transport_fragment_prefix_t *value,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes);
ucn_result_t ucn_i_transport_fragment_prefix_decode(
    const uint8_t *input,
    size_t input_bytes,
    uint8_t parent_kind,
    ucn_i_transport_fragment_prefix_t *value_out);
ucn_result_t ucn_i_transport_sack_encode(
    const ucn_i_transport_sack_t *value,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes);
ucn_result_t ucn_i_transport_sack_decode(
    const uint8_t *input,
    size_t input_bytes,
    uint8_t parent_kind,
    ucn_i_transport_sack_t *value_out);

ucn_result_t ucn_i_transport_owner_init(
    ucn_i_transport_owner_t *owner,
    const ucn_i_transport_config_t *config);
ucn_result_t ucn_i_transport_owner_destroy(ucn_i_transport_owner_t *owner);

ucn_result_t ucn_i_transport_reliable_begin(
    ucn_i_transport_owner_t *owner,
    const ucn_i_transport_reliable_key_t *key,
    const ucn_i_transport_security_facts_t *security,
    const uint8_t aad_digest[UCN_I_TRANSPORT_DIGEST_BYTES],
    const uint8_t payload_digest[UCN_I_TRANSPORT_DIGEST_BYTES],
    const uint8_t *sealed,
    uint16_t sealed_bytes,
    uint64_t now_us,
    ucn_handle_t *handle_out);
ucn_result_t ucn_i_transport_reliable_copy_attempt(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    uint64_t now_us,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes);
ucn_result_t ucn_i_transport_reliable_note_submit(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    bool submitted,
    uint64_t now_us);
ucn_result_t ucn_i_transport_reliable_accept_ack(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    const ucn_i_transport_delivery_ack_t *ack,
    const ucn_i_transport_ack_facts_t *facts,
    uint64_t now_us);
ucn_result_t ucn_i_transport_reliable_receive(
    ucn_i_transport_owner_t *owner,
    const ucn_i_transport_reliable_key_t *key,
    const ucn_i_transport_security_facts_t *security,
    const uint8_t aad_digest[UCN_I_TRANSPORT_DIGEST_BYTES],
    const uint8_t payload_digest[UCN_I_TRANSPORT_DIGEST_BYTES],
    uint8_t ack_status,
    uint8_t receive_credit,
    uint64_t now_us,
    ucn_i_transport_receive_action_t *action_out);
ucn_result_t ucn_i_transport_reliable_view(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    ucn_i_transport_reliable_view_t *view_out);
ucn_result_t ucn_i_transport_reliable_cancel(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle);
ucn_result_t ucn_i_transport_reliable_retire(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle);
ucn_result_t ucn_i_transport_maintain(
    ucn_i_transport_owner_t *owner,
    uint64_t now_us,
    uint16_t budget,
    uint16_t *inspected_out,
    uint16_t *changed_out);

ucn_result_t ucn_i_transport_transfer_tx_begin(
    ucn_i_transport_owner_t *owner,
    const ucn_i_transport_transfer_setup_t *setup,
    const ucn_i_transport_transfer_facts_t *facts,
    const uint8_t *message,
    uint32_t message_bytes,
    uint64_t now_us,
    ucn_handle_t *handle_out);
ucn_result_t ucn_i_transport_c1_transfer_tx_begin(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    const ucn_i_transport_transfer_setup_t *setup,
    const ucn_i_transport_transfer_facts_t *facts,
    const uint8_t *message,
    uint32_t message_bytes,
    uint64_t now_us,
    ucn_handle_t *handle_out);
ucn_result_t ucn_i_transport_transfer_tx_accept_setup(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    const ucn_i_transport_transfer_setup_t *setup,
    const ucn_i_transport_transfer_facts_t *response_facts,
    uint64_t now_us);
ucn_result_t ucn_i_transport_transfer_tx_fragment_copy(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    uint16_t fragment_index,
    uint64_t now_us,
    ucn_i_transport_fragment_prefix_t *prefix_out,
    uint8_t *payload_out,
    size_t payload_capacity,
    size_t *payload_bytes);
ucn_result_t ucn_i_transport_transfer_tx_accept_sack(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    const ucn_i_transport_sack_t *sack,
    const ucn_i_transport_transfer_facts_t *response_facts,
    uint64_t now_us);
ucn_result_t ucn_i_transport_transfer_rx_setup(
    ucn_i_transport_owner_t *owner,
    const ucn_i_transport_transfer_setup_t *setup,
    const ucn_i_transport_transfer_facts_t *facts,
    uint64_t now_us,
    ucn_handle_t *handle_out,
    bool *exact_duplicate_out);
ucn_result_t ucn_i_transport_c1_transfer_rx_setup(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    const ucn_i_transport_transfer_setup_t *setup,
    const ucn_i_transport_transfer_facts_t *facts,
    uint64_t now_us,
    ucn_handle_t *handle_out,
    bool *exact_duplicate_out);
ucn_result_t ucn_i_transport_transfer_rx_fragment(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    const ucn_i_transport_fragment_prefix_t *prefix,
    const ucn_i_transport_transfer_facts_t *facts,
    const uint8_t *payload,
    uint16_t payload_bytes,
    const uint8_t aad_digest[UCN_I_TRANSPORT_DIGEST_BYTES],
    uint64_t now_us,
    bool *exact_duplicate_out);
ucn_result_t ucn_i_transport_transfer_rx_sack(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    uint16_t window_base,
    uint16_t receive_credit,
    ucn_i_transport_sack_t *sack_out);
ucn_result_t ucn_i_transport_transfer_rx_copy_complete(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes);
ucn_result_t ucn_i_transport_transfer_rx_mark_delivered(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    uint64_t now_us);
ucn_result_t ucn_i_transport_transfer_view(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    ucn_i_transport_transfer_view_t *view_out);
ucn_result_t ucn_i_transport_transfer_retire(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle);
ucn_result_t ucn_i_transport_transfer_abort(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle);
ucn_result_t ucn_i_transport_terminal_receipt_view(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    uint64_t now_us,
    ucn_i_transport_terminal_receipt_view_t *view_out);

ucn_result_t ucn_i_transport_parent_record_encode(
    const ucn_i_transport_parent_context_t *context,
    uint32_t transfer_high_water,
    uint64_t foundation_transaction_id,
    uint8_t output[UCN_I_TRANSPORT_PARENT_RECORD_BYTES]);
ucn_result_t ucn_i_transport_parent_record_decode(
    const uint8_t input[UCN_I_TRANSPORT_PARENT_RECORD_BYTES],
    ucn_i_transport_parent_context_t *context_out,
    uint32_t *transfer_high_water_out,
    uint64_t *foundation_transaction_id_out);
ucn_result_t ucn_i_transport_parent_import(
    ucn_i_transport_owner_t *owner,
    const ucn_i_transport_parent_context_t *context,
    uint32_t transfer_high_water,
    uint64_t domain_id,
    uint64_t record_generation,
    uint64_t foundation_transaction_id,
    uint16_t domain_generation,
    const uint8_t published_body_digest[UCN_I_TRANSPORT_DIGEST_BYTES],
    ucn_handle_t *handle_out);
ucn_result_t ucn_i_transport_parent_prepare_first(
    ucn_i_transport_owner_t *owner,
    const ucn_i_transport_parent_context_t *context,
    const ucn_i_transport_parent_durability_base_t *durability,
    uint64_t now_us,
    ucn_handle_t *handle_out);
ucn_result_t ucn_i_transport_parent_prepare_next(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    const ucn_i_transport_parent_durability_base_t *durability,
    uint64_t now_us);
ucn_result_t ucn_i_transport_parent_requirement_get(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    ucn_i_transport_parent_requirement_t *requirement_out);
ucn_result_t ucn_i_transport_parent_bind_persistence(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    ucn_handle_t persistence_handle,
    const uint8_t expected_published_digest[UCN_I_TRANSPORT_DIGEST_BYTES]);
ucn_result_t ucn_i_transport_parent_activate_next(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    const ucn_i_transport_parent_proof_t *proof,
    uint64_t now_us,
    uint32_t *transfer_id_out);
ucn_result_t ucn_i_transport_parent_discard_grant(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    uint32_t transfer_id);
ucn_result_t ucn_i_transport_parent_abort_unsubmitted(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent);
ucn_result_t ucn_i_transport_parent_fail_persistence(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    const ucn_i_transport_parent_failure_t *failure);
ucn_result_t ucn_i_transport_parent_view(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    ucn_i_transport_parent_view_t *view_out);

#endif
