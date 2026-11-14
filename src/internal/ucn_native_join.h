#ifndef UCN_INTERNAL_NATIVE_JOIN_H
#define UCN_INTERNAL_NATIVE_JOIN_H

#include "internal/ucn_admission.h"
#include "internal/ucn_adapter.h"
#include "internal/ucn_security.h"
#include "internal/ucn_persistence.h"

/* Fixed trusted pair only. Authority is explicitly provisioned/current; this
 * coordinator does not manufacture or recover an Authority Record. */
typedef ucn_result_t (*ucn_i_native_join_authority_fn)(void *, uint64_t,
    ucn_i_identity_authority_view_t *);
/* Produce and sign the next canonical event using the product identity key.
 * Output is untrusted until phase/domain checks and authorize_event succeed. */
typedef ucn_result_t (*ucn_i_native_join_sign_fn)(void *,
    ucn_i_admission_event_t, const ucn_i_admission_transcript_t *,
    ucn_i_admission_transcript_t *, ucn_i_admission_evidence_t *);
/* Borrowed immutable exact rules must outlive the resulting Security Session.
 * This explicit product policy is never derived from remote permission claims. */
typedef ucn_result_t (*ucn_i_native_join_policy_fn)(void *,
    const ucn_i_security_candidate_t *, const ucn_i_security_acl_rule_t **, uint16_t *);

typedef struct ucn_i_native_join_config {
    ucn_i_adapter_t *adapter;
    ucn_i_admission_owner_t *admission; /* Authority only; NULL on Device. */
    ucn_i_identity_owner_t *identity;
    ucn_i_security_owner_t *security;
    ucn_persistence_owner_t *persistence;
    ucn_i_callback_gate_t *callback_gate;
    ucn_i_admission_provider_t authentication;
    void *product_context;
    ucn_i_native_join_authority_fn current_authority;
    ucn_i_native_join_sign_fn sign_event;
    ucn_i_native_join_policy_fn configure_session_policy;
    ucn_i_identity_handle_t authority_binding;
    uint64_t binding_domain_id, session_domain_id;
    uint64_t transaction, device_nonce, freshness_nonce;
    uint64_t timeout_us, session_lifetime_us, retry_interval_us, cookie_bucket_us;
    uint32_t realm_id, policy_generation, physical_peer;
    uint32_t device_address, device_binding_generation;
    uint32_t link_generation;
    uint16_t link_index, owner_instance, max_attempts;
    uint8_t address_width, device;
    uint8_t device_principal[16], authority_principal[16], identity_digest[16];
} ucn_i_native_join_config_t;

typedef struct ucn_i_native_join_current {
    ucn_i_security_handle_t session;
    ucn_i_security_candidate_t candidate;
    ucn_i_identity_authority_view_t authority;
    ucn_i_identity_binding_view_t local, peer;
} ucn_i_native_join_current_t;

/* Caller owns fixed storage. All entry points are serialized by the product;
 * product/Owner/Driver callbacks share one actual callback gate. No heap.
 * Config, Owners, gates and policy arrays are disjoint borrowed storage: retain
 * them unchanged through stop/drain or the entire handed-off continuation. */
typedef struct ucn_i_native_join {
    uint32_t magic;
    ucn_i_native_join_config_t config;
    const ucn_i_native_join_config_t *configuration_source; /* Borrowed until drain. */
    const ucn_i_security_acl_rule_t *session_acl;
    uint16_t session_acl_count;
    uint64_t started_us, now_us, deadline_us, retry_at_us;
    uint64_t cookie_verify_refill_us;
    uint32_t operation_id;
    uint8_t phase, stopped, handed_off, work_kind;
    uint8_t tx_live, tx_ready, tx_index, tx_count;
    uint8_t tx_opcode, tx_round_complete, hello_submitted, echo_complete;
    uint8_t rx_phase, rx_count, rx_chunk, active;
    uint8_t binding_ready, requeue, failed, proof_consumed;
    uint8_t cookie_verify_tokens;
    uint16_t attempts, rx_bitmap, rx_total, tx_total;
    ucn_driver_token_t tx_token;
    ucn_i_adapter_tx_view_t tx_view;
    ucn_i_admission_handle_t admission_handle;
    ucn_i_identity_handle_t device_binding;
    ucn_i_security_handle_t session;
    ucn_handle_t persistence_handle;
    ucn_i_identity_requirement_view_t binding_requirement;
    ucn_i_security_requirement_view_t session_requirement;
    ucn_i_admission_transcript_t transcript, proposed;
    ucn_i_admission_evidence_t evidence, proposed_evidence;
    ucn_i_admission_hello_t hello;
    ucn_i_admission_cookie_challenge_t challenge;
    ucn_i_admission_hello_cookie_t echo;
    ucn_i_native_join_current_t current;
    ucn_i_identity_authority_view_t frozen_authority;
    ucn_i_identity_binding_view_t frozen_authority_binding, frozen_device_binding;
    ucn_i_security_current_facts_t facts;
    ucn_i_security_session_view_t session_view;
    ucn_i_admission_pending_view_t pending;
    ucn_i_admitted_view_t admitted;
    ucn_i_callback_claim_t claim;
    uint8_t tx_logical[508], rx_logical[508];
    uint8_t tx_frame[UCN_ADAPTER_FRAME_BYTES];
    size_t tx_frame_bytes;
    /* These phases never overlap and do not escape synchronous calls. */
    union {
        struct {
            ucn_i_c0_frame_t frame;
            ucn_i_admission_fragment_t fragment;
            ucn_i_admission_wire_workspace_t decode;
            uint8_t payload[164], hash_input[196];
            ucn_i_sha256_workspace_t hash;
        } wire;
        ucn_i_admission_seed_workspace_t seed;
        struct {
            ucn_persistence_domain_view_t domain;
            ucn_persistence_request_t request;
            ucn_persistence_request_view_t request_view;
            ucn_persistence_proof_t proof;
            ucn_i_persist_codec_workspace_t codec;
            ucn_i_persist_record_meta_t meta;
            ucn_i_sha256_workspace_t hash;
            ucn_i_identity_binding_issue_t issue;
            ucn_i_identity_durability_base_t binding_base;
            ucn_i_security_durability_base_t session_base;
            ucn_i_identity_requirement_view_t binding_requirement;
            ucn_i_security_requirement_view_t session_requirement;
            ucn_i_identity_durability_proof_t binding_proof;
            ucn_i_identity_persistence_failure_t binding_failure;
            ucn_i_security_durability_proof_t session_proof;
            uint8_t expected_digest[16], body[UCN_I_SECURITY_RECORD_BYTES];
            size_t body_bytes;
        } durability;
    } scratch;
} ucn_i_native_join_t;

ucn_result_t ucn_i_native_join_init(ucn_i_native_join_t *,
    const ucn_i_native_join_config_t *, uint64_t now_us);
/* The caller owns and retires the exact already-claimed atomic RX token.
 * This function never claims, retires, requeues, or synthesizes an RX item. */
ucn_result_t ucn_i_native_join_receive_claimed(ucn_i_native_join_t *,
    const ucn_i_adapter_rx_view_t *, uint64_t now_us);
/* At most one physical submission, one phase, or one persistence action. */
ucn_result_t ucn_i_native_join_drive_one(ucn_i_native_join_t *, uint64_t now_us);
ucn_result_t ucn_i_native_join_finish_durability(ucn_i_native_join_t *, uint64_t now_us);
ucn_result_t ucn_i_native_join_retire_one(ucn_i_native_join_t *, uint64_t now_us);
ucn_result_t ucn_i_native_join_session_current(ucn_i_native_join_t *, uint64_t,
    ucn_i_native_join_current_t *);
/* Revalidate frozen Identity parents only, including after handoff/session fence.
 * The returned candidate/session fields are NOT an ACTIVE Session proof. */
ucn_result_t ucn_i_native_join_bindings_current(ucn_i_native_join_t *, uint64_t,
    ucn_i_native_join_current_t *);
/* Once-only ownership transfer after current parents and actual holders drain.
 * Success disables every old network/stop entry, not the shared Adapter RX. */
ucn_result_t ucn_i_native_join_handoff(ucn_i_native_join_t *, uint64_t,
    ucn_i_native_join_current_t *);
ucn_result_t ucn_i_native_join_stop(ucn_i_native_join_t *, uint64_t now_us);
/* Own TX/Foundation/Admission obligations only; raw RX is caller-owned.
 * A stopped nonterminal Admission waits for its original absolute deadline;
 * retire_one expires at most one Owner slot then retires its exact terminal.
 * Global Adapter drainage is additionally enforced by handoff. */
ucn_result_t ucn_i_native_join_quiescent(ucn_i_native_join_t *, bool *);

/* Internal split-phase implementation; never callable as authorization DTOs. */
ucn_result_t ucn_i_native_join_check(ucn_i_native_join_t *, uint64_t, bool);
ucn_result_t ucn_i_native_join_retirement_check(ucn_i_native_join_t *, uint64_t);
ucn_result_t ucn_i_native_join_configure_policy(ucn_i_native_join_t *);
ucn_result_t ucn_i_native_join_parents(ucn_i_native_join_t *, uint64_t, bool);
ucn_result_t ucn_i_native_join_persist_binding(ucn_i_native_join_t *, uint64_t);
ucn_result_t ucn_i_native_join_persist_session(ucn_i_native_join_t *, uint64_t);

#endif
