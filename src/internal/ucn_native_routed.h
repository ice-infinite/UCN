#ifndef UCN_INTERNAL_NATIVE_ROUTED_H
#define UCN_INTERNAL_NATIVE_ROUTED_H
#include "ucn/ucn_native_backend.h"
#include "internal/ucn_security.h"
#include "internal/ucn_adapter.h"
#include "internal/ucn_route.h"

#define UCN_I_NATIVE_ENDPOINTS 4U
#define UCN_I_NATIVE_NEIGHBORS 4U
#define UCN_I_NATIVE_FORWARD_DOMAINS 4U
struct ucn_i_native_route_control;
typedef ucn_result_t (*ucn_i_native_c0_reserve_fn)(void *,
    ucn_i_security_owner_t *, ucn_i_security_handle_t, uint64_t *);
/* Sealed Static identity assembly only: no mutable Parent, no hot replacement.
 * Product must fence/drain all borrowers before replacing any binding. All
 * Owners remain borrowed and share ONE provider gate until physical drain.
 * One serialized Coordinator is the exclusive Adapter RX/retirement owner.
 * Adapter runtime_instance is its immutable Scope namespace: never reuse it
 * for a successor Coordinator or initialize competing roots on that Adapter.
 * The fixed endpoint slot generation is 1 because this assembly has no reuse.
 * Each Neighbor requires a dedicated Link: Route link_id = Adapter slot + 1.
 * sender_discriminator must match RX exactly (use 0 for a dedicated transport
 * which has no discriminator); no shared-Link destination guessing is allowed.
 * Absolute assembly expiry seals Static identity/capability facts. Session/ACL
 * and physical Link generations are still checked live for every admission.
 * control == NULL consumes published Route Owner data only. The optional
 * fixed control Coordinator performs authenticated dynamic route discovery;
 * it does not turn this sealed assembly into dynamic identity admission. */
typedef struct ucn_i_native_endpoint {
    ucn_i_security_owner_t *security;
    ucn_i_security_handle_t session;
    ucn_i_security_current_facts_t facts;
    ucn_native_source_t source;
    uint8_t fingerprint[16];
    uint16_t service;
    uint8_t minimum, reserved_zero;
} ucn_i_native_endpoint_t;
typedef struct ucn_i_native_neighbor {
    ucn_i_security_owner_t *security;
    ucn_i_security_handle_t session;
    ucn_i_security_current_facts_t facts;
    uint32_t sender_discriminator;
    uint8_t fingerprint[16];
    uint16_t link_slot;
    uint16_t capability_bits;
    uint8_t control_fingerprint[16];
} ucn_i_native_neighbor_t;
typedef struct ucn_i_native_routed_config {
    ucn_i_adapter_t *adapter;
    ucn_i_route_owner_t *routes;
    ucn_i_callback_gate_t *gate;
    ucn_native_source_t local;
    uint64_t now_us, expires_at_us, relay_lifetime_us;
    const ucn_i_native_endpoint_t *endpoints;
    const ucn_i_native_neighbor_t *neighbors;
    const ucn_i_route_domain_t *forward_domains;
    uint16_t endpoint_count, neighbor_count, forward_count;
    uint8_t hop_limit, reserved_zero;
    struct ucn_i_native_route_control *control;
    ucn_i_native_c0_reserve_fn reserve_c0;
    void *counter_context;
} ucn_i_native_routed_config_t;

typedef struct ucn_i_native_routed {
    ucn_native_backend_t port;
    ucn_i_adapter_t *adapter;
    ucn_i_route_owner_t *routes;
    ucn_i_callback_gate_t *gate;
    struct ucn_i_native_route_control *control;
    ucn_native_source_t local;
    uint64_t now, expires_at, relay_lifetime, tx_deadline;
    uint32_t instance;
    uint16_t endpoint_count, neighbor_count, forward_count;
    uint8_t width, hop_limit, lifecycle;
    ucn_i_native_endpoint_t endpoints[UCN_I_NATIVE_ENDPOINTS];
    ucn_i_native_neighbor_t neighbors[UCN_I_NATIVE_NEIGHBORS];
    ucn_i_route_domain_t forward_domains[UCN_I_NATIVE_FORWARD_DOMAINS];
    ucn_i_security_current_facts_t current_facts;
    ucn_i_security_access_request_t access;
    ucn_i_security_session_view_t session_view;
    ucn_i_route_domain_t domain, tx_domain;
    ucn_i_route_forward_key_t route_key;
    ucn_i_route_resolved_t resolved;
    ucn_i_security_packet_workspace_t workspace;
    union {
        ucn_i_security_c1_origin_material_t material;
        ucn_i_security_c1_open_call_t open;
        ucn_i_callback_claim_t callback;
    } open_work;
    ucn_i_security_tx_handle_t origin_tx;
    ucn_i_security_replay_handle_t origin_replay;
    ucn_i_security_hop_replay_handle_t hop_replay;
    uint8_t origin[UCN_ADAPTER_FRAME_BYTES], frame[UCN_ADAPTER_FRAME_BYTES];
    uint8_t plaintext[UCN_NATIVE_PAYLOAD_BYTES];
    size_t origin_bytes, frame_bytes, plaintext_bytes;
    ucn_driver_token_t tx;
    ucn_i_adapter_tx_view_t tx_view;
    ucn_i_adapter_rx_view_t rx;
    ucn_native_backend_message_t received;
    uint16_t tx_neighbor, tx_endpoint, tx_service;
    uint16_t tx_payload_bytes;
    uint8_t tx_live, tx_prepared, rx_live, rx_ready, tx_pending, tx_traffic_class;
    uint32_t rx_source;
    uint16_t rx_service;
    uint8_t rx_endpoint_pending;
} ucn_i_native_routed_t;

ucn_result_t ucn_i_native_routed_init(ucn_i_native_routed_t *,
    const ucn_i_native_routed_config_t *);
#endif
