#ifndef UCN_INTERNAL_NATIVE_REAUTH_H
#define UCN_INTERNAL_NATIVE_REAUTH_H
#include "internal/ucn_native_join.h"

#define UCN_I_NATIVE_REAUTH_MESSAGE_BYTES 227U
#define UCN_I_NATIVE_REAUTH_LOGICAL_BYTES 273U
/* Exact message = ASCII UCN6-PEER-REAUTH-V2 || canonical Record208.
 * Product key resolution is fixed per Principal, never fallback algorithms. */
typedef ucn_result_t (*ucn_i_native_reauth_sign_fn)(void *, const uint8_t[16],
    const uint8_t[UCN_I_NATIVE_REAUTH_MESSAGE_BYTES], uint8_t[64], uint8_t *);
typedef ucn_result_t (*ucn_i_native_reauth_verify_fn)(void *, const uint8_t[16],
    const uint8_t[UCN_I_NATIVE_REAUTH_MESSAGE_BYTES], const uint8_t *, uint8_t);
typedef struct ucn_i_native_reauth_config {
    ucn_i_native_join_t *parents; /* Original handed-off root; never reconstructed. */
    void *product_context;
    ucn_i_native_reauth_sign_fn sign;
    ucn_i_native_reauth_verify_fn verify;
    ucn_i_native_join_policy_fn configure_session_policy;
    uint64_t nonce, transaction; /* Responder transaction must be zero. */
    uint64_t timeout_us, session_lifetime_us, retry_interval_us;
    uint32_t policy_generation;
    uint16_t owner_instance, max_attempts;
    uint8_t initiator, proof_bytes;
} ucn_i_native_reauth_config_t;

/* Caller-owned fixed workspace; all calls serialized by the outer Coordinator.
 * All config/parent/Owner/gate/policy borrows survive physical drain and every
 * use of the successor Session. Raw RX is exclusively owned by the outer root. */
typedef struct ucn_i_native_reauth {
    uint32_t magic, operation_id;
    ucn_i_native_reauth_config_t config;
    const ucn_i_native_reauth_config_t *configuration_source;
    uint64_t started_us, now_us, deadline_us, retry_at_us;
    uint8_t phase, stopped, failed, active, work, proof_consumed, handed_off;
    uint8_t tx_live, tx_ready, tx_stage, tx_index, tx_count, tx_complete;
    uint8_t rx_stage, rx_bitmap, rx_count, peer_observed;
    uint16_t attempts, tx_bytes, rx_bytes;
    uint16_t callbacks_remaining;
    uint64_t rx_transaction;
    ucn_driver_token_t tx_token;
    ucn_i_adapter_tx_view_t tx_view;
    ucn_i_callback_claim_t claim;
    ucn_i_native_join_current_t current;
    ucn_i_security_candidate_t previous, candidate;
    ucn_i_security_durability_base_t base;
    ucn_i_security_handle_t session;
    ucn_handle_t persistence_handle;
    ucn_i_security_requirement_view_t requirement;
    const ucn_i_security_acl_rule_t *session_acl;
    uint16_t session_acl_count;
    ucn_i_peer_reauth_record_t record, incoming;
    uint8_t challenge_proof[64], response_proof[64];
    uint8_t tx_logical[273], rx_logical[273];
    uint8_t tx_frame[UCN_ADAPTER_FRAME_BYTES];
    size_t tx_frame_bytes;
    union {
        struct {
            ucn_i_c0_frame_t frame;
            ucn_i_peer_reauth_fragment_t fragment;
            uint8_t payload[98];
        } wire;
        struct {
            uint8_t message[227], proof[64];
            ucn_i_sha256_workspace_t hash;
        } crypto;
        struct {
            ucn_persistence_domain_view_t domain;
            ucn_persistence_request_t request;
            ucn_persistence_request_view_t view;
            ucn_persistence_proof_t proof;
            ucn_i_security_durability_proof_t projected;
            ucn_i_security_codec_workspace_t security_codec;
            ucn_i_security_reauth_call_t call;
            ucn_i_security_reauth_workspace_t reauth;
            ucn_i_persist_codec_workspace_t codec;
            ucn_i_persist_record_meta_t meta;
            ucn_i_security_candidate_t decoded;
            ucn_i_security_current_facts_t facts;
            uint8_t body[192], expected_digest[16];
        } durable;
    } scratch;
} ucn_i_native_reauth_t;

ucn_result_t ucn_i_native_reauth_init(ucn_i_native_reauth_t *, const ucn_i_native_reauth_config_t *, uint64_t);
ucn_result_t ucn_i_native_reauth_receive_claimed(ucn_i_native_reauth_t *, const ucn_i_adapter_rx_view_t *, uint64_t);
ucn_result_t ucn_i_native_reauth_drive_one(ucn_i_native_reauth_t *, uint64_t);
ucn_result_t ucn_i_native_reauth_retire_one(ucn_i_native_reauth_t *, uint64_t);
ucn_result_t ucn_i_native_reauth_finish_durability(ucn_i_native_reauth_t *, uint64_t);
/* Established only: Initiator requires authenticated ACCEPT; Responder requires
 * its last ACCEPT physical token retired. Keeps its frozen reply cache alive. */
ucn_result_t ucn_i_native_reauth_session_current(ucn_i_native_reauth_t *, uint64_t, ucn_i_native_join_current_t *);
ucn_result_t ucn_i_native_reauth_bindings_current(ucn_i_native_reauth_t *, uint64_t, ucn_i_native_join_current_t *);
/* Trusted outer Coordinator calls only after actual authenticated current
 * Capability admission proves the peer observed this successor. Transfers an
 * established ACTIVE Session without fencing it; own TX/IO must truly drain. */
ucn_result_t ucn_i_native_reauth_handoff(ucn_i_native_reauth_t *, uint64_t, ucn_i_native_join_current_t *);
ucn_result_t ucn_i_native_reauth_stop(ucn_i_native_reauth_t *, uint64_t);
ucn_result_t ucn_i_native_reauth_quiescent(ucn_i_native_reauth_t *, bool *);

/* Split implementation helpers, never authenticated external DTO entry points. */
ucn_result_t ucn_i_native_reauth_check(ucn_i_native_reauth_t *, uint64_t, bool);
ucn_result_t ucn_i_native_reauth_enter(ucn_i_native_reauth_t *);
ucn_result_t ucn_i_native_reauth_provider_enter(ucn_i_native_reauth_t *);
ucn_result_t ucn_i_native_reauth_leave(ucn_i_native_reauth_t *, ucn_result_t);
ucn_result_t ucn_i_native_reauth_load_base(ucn_i_native_reauth_t *);
ucn_result_t ucn_i_native_reauth_persist(ucn_i_native_reauth_t *, uint64_t);
ucn_result_t ucn_i_native_reauth_candidate(ucn_i_native_reauth_t *);

#endif
