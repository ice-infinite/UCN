#ifndef UCN_INTERNAL_NATIVE_CAPABILITY_H
#define UCN_INTERNAL_NATIVE_CAPABILITY_H

#include "internal/ucn_capability.h"
#include "internal/ucn_native_join.h"
#include "internal/ucn_native_routed.h"

/* Private fixed-peer Coordinator: serialized, caller-owned, no heap. The caller
 * exclusively claims and retires Adapter RX; this leaf never takes ownership. */
typedef struct ucn_i_native_capability_config {
    ucn_i_adapter_t *adapter;
    ucn_i_security_owner_t *security;
    ucn_i_capability_owner_t *capability_owner;
    ucn_i_native_join_t *join_parents;
    ucn_i_callback_gate_t *callback_gate;
    ucn_i_security_handle_t session;
    ucn_i_security_candidate_t candidate;
    ucn_i_capability_record_t local_record;
    ucn_i_profile_requirements_t requirements;
    ucn_i_capability_ref_t predecessor;
    ucn_i_native_c0_reserve_fn reserve_c0;
    void *counter_context;
    uint64_t deadline_us, retry_interval_us;
    uint32_t physical_peer, link_generation;
    uint16_t link_slot, owner_instance, max_attempts, service;
    uint8_t device, has_predecessor;
} ucn_i_native_capability_config_t;

typedef struct ucn_i_native_capability {
    uint32_t magic, operation_id;
    ucn_i_native_capability_config_t config;
    uint64_t now_us, retry_at_us, tx_transaction[2], rx_base, rx_highest_end;
    ucn_i_native_join_current_t parents;
    ucn_i_security_current_facts_t facts;
    ucn_i_security_session_view_t session_view;
    ucn_i_security_candidate_t actual_candidate;
    ucn_i_authenticated_peer_view_t peer;
    ucn_i_capability_ref_t reference;
    ucn_i_cached_capability_t cached;
    ucn_i_capability_record_t remote_record;
    ucn_i_profile_select_t profile;
    ucn_i_profile_ack_t ack;
    ucn_i_capability_select_context_t select_context;
    ucn_i_capability_fragment_t fragment;
    ucn_i_capability_summary_t summary;
    ucn_i_capability_query_t query;
    ucn_i_c0_frame_t rx_frame;
    uint8_t remote_digest[16];
    ucn_i_security_control_access_request_t control_access;
    ucn_i_security_c0_protect_request_t protect_request;
    ucn_i_sha256_workspace_t hash;
    ucn_i_security_packet_workspace_t packet_work;
    ucn_i_security_c0_path_workspace_t path_work;
    ucn_i_security_authenticated_control_t authenticated;
    ucn_i_security_replay_handle_t origin_replay;
    ucn_i_security_hop_replay_handle_t hop_replay;
    ucn_i_callback_claim_t claim;
    ucn_driver_token_t tx_token;
    ucn_i_adapter_tx_view_t tx_view;
    uint8_t tx_frame[2][UCN_ADAPTER_FRAME_BYTES];
    uint8_t tx_refresh[UCN_ADAPTER_FRAME_BYTES];
    uint8_t select_origin[UCN_ADAPTER_FRAME_BYTES];
    size_t select_origin_bytes;
    uint8_t fragment_origin[2][UCN_ADAPTER_FRAME_BYTES];
    size_t fragment_origin_bytes[2];
    size_t tx_refresh_bytes;
    uint8_t actual_candidate_record[UCN_I_SECURITY_RECORD_BYTES];
    uint8_t expected_candidate_record[UCN_I_SECURITY_RECORD_BYTES];
    size_t tx_bytes[2];
    size_t tx_body_bytes, rx_body_bytes;
    uint8_t body[UCN_I_PROFILE_ACK_BYTES], rx_body[UCN_I_PROFILE_ACK_BYTES], plaintext[UCN_I_PROFILE_ACK_BYTES];
    uint8_t fragment_bytes[UCN_I_CAPABILITY_FRAGMENT_PREFIX + UCN_I_CAPABILITY_FRAGMENT_MAX_CHUNK];
    uint8_t local_digest[16], select_digest[16];
    uint16_t attempts, tx_opcode, rx_opcode, cached_request_opcode, cached_reply_opcode;
    uint8_t phase, stopped, selected_ready, tx_live, tx_count, tx_index;
    uint8_t tx_ready, tx_refreshed, rx_bitmap, rx_role, pending_control, requeue, fragment_origin_bitmap;
} ucn_i_native_capability_t;

ucn_result_t ucn_i_native_capability_init(ucn_i_native_capability_t *,
    const ucn_i_native_capability_config_t *, uint64_t now_us);
ucn_result_t ucn_i_native_capability_receive_claimed(ucn_i_native_capability_t *,
    const ucn_i_adapter_rx_view_t *, uint64_t now_us);
ucn_result_t ucn_i_native_capability_drive_one(ucn_i_native_capability_t *, uint64_t now_us);
ucn_result_t ucn_i_native_capability_retire_one(ucn_i_native_capability_t *, uint64_t now_us);
ucn_result_t ucn_i_native_capability_selected(ucn_i_native_capability_t *, uint64_t now_us,
    ucn_i_capability_ref_t *, ucn_i_cached_capability_t *, ucn_i_profile_select_t *);
ucn_result_t ucn_i_native_capability_stop(ucn_i_native_capability_t *, uint64_t now_us);
ucn_result_t ucn_i_native_capability_quiescent(ucn_i_native_capability_t *, bool *);

#endif
