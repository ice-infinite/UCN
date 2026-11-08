#ifndef UCN_INTERNAL_NATIVE_SESSION_H
#define UCN_INTERNAL_NATIVE_SESSION_H
#include "ucn/ucn_native_backend.h"
#include "internal/ucn_security.h"
#include "internal/ucn_adapter.h"

/* Coordinator adapter for one immutable, durably activated, direct H0 Session.
 * 单个不可变、已耐久激活的直连H0 Session协调器适配器；不冒充动态路由。 */
typedef struct ucn_i_native_session_backend {
    ucn_native_backend_t port;
    ucn_i_security_owner_t *security;
    ucn_i_adapter_t *adapter;
    ucn_i_security_handle_t session;
    ucn_i_security_current_facts_t facts;
    ucn_i_security_access_request_t access;
    ucn_i_security_session_view_t session_view;
    ucn_native_source_t local, peer;
    uint8_t fingerprint[16];
    uint16_t service;
    uint8_t width, lifecycle;
    uint64_t now, tx_deadline;
    ucn_driver_token_t tx;
    ucn_i_adapter_tx_view_t tx_view;
    uint8_t tx_live, tx_prepared;
    uint8_t frame[UCN_ADAPTER_FRAME_BYTES];
    size_t frame_bytes;
    ucn_i_security_packet_workspace_t workspace;
    union {
        ucn_i_security_c1_origin_material_t material;
        ucn_i_security_c1_open_call_t open;
        ucn_i_callback_claim_t callback;
    } open_work;
    ucn_i_security_tx_handle_t security_tx;
    ucn_i_adapter_rx_view_t rx;
    ucn_i_security_replay_handle_t replay;
    ucn_native_backend_message_t received;
    uint8_t plaintext[UCN_NATIVE_PAYLOAD_BYTES];
    size_t plaintext_bytes;
    uint8_t rx_live, rx_ready;
} ucn_i_native_session_backend_t;

/* Exclusive product assembly, zero context, owners remain borrowed until drain.
 * 独占产品装配，全零context；Owner借用持续到真实排空。 */
ucn_result_t ucn_i_native_session_init(ucn_i_native_session_backend_t *,
    ucn_i_security_owner_t *, ucn_i_adapter_t *, ucn_i_security_handle_t,
    const ucn_i_security_current_facts_t *, const uint8_t[16], uint8_t,
    uint16_t, uint64_t, uint64_t);
#endif
