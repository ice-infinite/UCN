#ifndef UCN_INTERNAL_NATIVE_ROUTE_CONTROL_H
#define UCN_INTERNAL_NATIVE_ROUTE_CONTROL_H
#include "internal/ucn_native_routed.h"
#define UCN_I_NATIVE_CONTROL_MAX_ATTEMPTS 16U

/* Caller owned, zero initialized, exclusively borrowed by one routed root.
 * Counter callback is product-owned, exact Owner+Session shared across every
 * C0 consumer. It is bounded/non-reentrant and must not call protocol APIs.
 * No pointer returned by a callback is retained. No mutable admission exists. */
typedef struct ucn_i_native_control_job {
    uint8_t body[99], frame[UCN_ADAPTER_FRAME_BYTES];
    size_t frame_bytes;
    uint64_t deadline, bundle;
    ucn_handle_t completion;
    ucn_driver_token_t token;
    uint16_t opcode, length;
    uint8_t occupied, live, prepared, piece, count, total_attempts;
} ucn_i_native_control_job_t;
typedef struct ucn_i_native_control_rx {
    uint8_t body[99];
    uint64_t high_bundle, bundle, deadline, received_at;
    uint16_t opcode, total;
    uint8_t count, mask;
} ucn_i_native_control_rx_t;
typedef struct ucn_i_native_control_discovery {
    ucn_handle_t handle;
    ucn_i_route_rreq_message_t request;
    uint64_t deadline;
    uint8_t occupied;
} ucn_i_native_control_discovery_t;
typedef struct ucn_i_native_control_input {
    uint8_t body[99];
    uint64_t received_at;
    uint16_t opcode, length;
    uint8_t fragmented;
} ucn_i_native_control_input_t;
typedef struct ucn_i_native_control_receipt {
    ucn_i_route_soft_view_t route;
    uint16_t upstream;
    uint8_t occupied, error_pending;
} ucn_i_native_control_receipt_t;
typedef struct ucn_i_native_route_control {
    ucn_i_native_c0_reserve_fn reserve;
    void *counter_context;
    ucn_i_native_routed_t *root;
    ucn_i_native_control_job_t jobs[UCN_I_NATIVE_NEIGHBORS];
    ucn_i_native_control_rx_t rx[UCN_I_NATIVE_NEIGHBORS];
    ucn_i_native_control_input_t input[UCN_I_NATIVE_NEIGHBORS];
    ucn_i_native_control_discovery_t discoveries[UCN_I_ROUTE_DISCOVERY_COUNT];
    ucn_i_native_control_receipt_t receipts[UCN_I_ROUTE_DYNAMIC_COUNT];
    ucn_i_security_packet_workspace_t packet;
    union {
        ucn_i_security_c0_protect_request_t protect;
        ucn_i_security_c0_open_call_t open;
    } crypto_work;
    ucn_i_security_control_access_request_t control_access;
    ucn_i_security_control_access_request_t access;
    ucn_i_security_current_facts_t facts;
    ucn_i_security_authenticated_control_t authenticated;
    ucn_i_security_session_view_t session;
    ucn_i_adapter_tx_view_t tx_view;
    ucn_i_route_link_ref_t ingress, upstream;
    ucn_i_route_rreq_message_t request;
    ucn_i_route_rrep_message_t reply;
    ucn_i_route_request_action_t request_action;
    ucn_i_route_reply_action_t reply_action;
    ucn_i_route_rerr_payload_t error;
    ucn_i_route_fragment_t fragment;
    uint8_t plaintext[99], encoded[99];
    size_t plaintext_bytes, encoded_bytes;
    uint32_t reporter_generation;
    uint64_t message_time;
    uint16_t tx_cursor, discovery_cursor, receipt_cursor, input_cursor;
} ucn_i_native_route_control_t;

ucn_result_t ucn_i_native_route_control_init(ucn_i_native_routed_t *,
    const ucn_i_native_routed_config_t *);
ucn_result_t ucn_i_native_route_control_discover(ucn_i_native_routed_t *,
    uint32_t target, uint16_t bytes, uint64_t deadline);
ucn_result_t ucn_i_native_route_control_receive(ucn_i_native_routed_t *);
ucn_result_t ucn_i_native_route_control_receive_context(ucn_i_native_routed_t *);
ucn_result_t ucn_i_native_route_control_receive_finish(ucn_i_native_routed_t *);
ucn_result_t ucn_i_native_route_control_step(ucn_i_native_routed_t *);
void ucn_i_native_route_control_dispatch(ucn_i_native_routed_t *);
void ucn_i_native_route_control_prepare(ucn_i_native_routed_t *);
void ucn_i_native_route_control_drive(ucn_i_native_routed_t *);
void ucn_i_native_route_control_discovery_step(ucn_i_native_routed_t *);
void ucn_i_native_route_control_receipt_step(ucn_i_native_routed_t *);
bool ucn_i_native_route_control_quiet(const ucn_i_native_routed_t *);
#endif
