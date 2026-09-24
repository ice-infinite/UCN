#ifndef UCN_INTERNAL_ROUTE_H
#define UCN_INTERNAL_ROUTE_H

#include "internal/ucn_owner.h"
#include "ucn/ucn_config.h"

#define UCN_I_ROUTE_SCHEMA UINT16_C(1)
#define UCN_I_ROUTE_RREQ_BYTES 9U
#define UCN_I_ROUTE_RREP_BYTES 29U
#define UCN_I_ROUTE_RERR_BYTES 99U
#define UCN_I_ROUTE_PRINCIPAL_BYTES 16U

#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_I_ROUTE_DISCOVERY_COUNT 2U
#define UCN_I_ROUTE_REVERSE_COUNT 4U
#define UCN_I_ROUTE_DYNAMIC_COUNT 4U
#define UCN_I_ROUTE_STATIC_COUNT 4U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_I_ROUTE_DISCOVERY_COUNT 4U
#define UCN_I_ROUTE_REVERSE_COUNT 12U
#define UCN_I_ROUTE_DYNAMIC_COUNT 16U
#define UCN_I_ROUTE_STATIC_COUNT 8U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define UCN_I_ROUTE_DISCOVERY_COUNT 8U
#define UCN_I_ROUTE_REVERSE_COUNT 32U
#define UCN_I_ROUTE_DYNAMIC_COUNT 48U
#define UCN_I_ROUTE_STATIC_COUNT 16U
#else
#error "UCN_PROFILE must be UCN_PROFILE_NANO, UCN_PROFILE_LITE, or UCN_PROFILE_FULL"
#endif

typedef struct ucn_i_route_binding {
    uint32_t address;
    uint32_t generation;
    uint8_t principal[UCN_I_ROUTE_PRINCIPAL_BYTES];
} ucn_i_route_binding_t;

typedef struct ucn_i_route_link_ref {
    ucn_i_route_binding_t peer;
    uint32_t link_generation;
    uint32_t cost;
    uint16_t link_id;
    uint16_t frame_mtu;
    uint16_t capability_bits;
    uint16_t reserved_zero;
} ucn_i_route_link_ref_t;

typedef struct ucn_i_route_domain {
    ucn_i_route_binding_t origin;
    ucn_i_route_binding_t destination;
    uint32_t realm;
    uint32_t origin_session_generation;
} ucn_i_route_domain_t;

typedef struct ucn_i_route_discovery_key {
    ucn_i_route_binding_t origin;
    uint64_t transaction_id;
    uint32_t realm;
    uint32_t origin_session_generation;
    uint32_t target_address;
} ucn_i_route_discovery_key_t;

typedef struct ucn_i_route_rreq_payload {
    uint32_t accumulated_cost;
    uint16_t minimum_payload_budget;
    uint16_t required_capability_bits;
    uint8_t flags;
    uint8_t reserved_zero[3];
} ucn_i_route_rreq_payload_t;

typedef struct ucn_i_route_rrep_payload {
    uint8_t destination_principal[UCN_I_ROUTE_PRINCIPAL_BYTES];
    uint32_t destination_binding_generation;
    uint32_t accumulated_cost;
    uint16_t path_frame_mtu;
    uint16_t capability_bits;
    uint8_t hop_count;
    uint8_t reserved_zero[3];
} ucn_i_route_rrep_payload_t;

typedef uint8_t ucn_i_route_rerr_reason_t;
enum {
    UCN_I_ROUTE_RERR_LINK_INVALID = 1,
    UCN_I_ROUTE_RERR_ROUTE_EXPIRED = 2,
    UCN_I_ROUTE_RERR_NEXT_HOP_REJECTED = 3,
    UCN_I_ROUTE_RERR_PATH_CONTRACT_CHANGED = 4
};

typedef struct ucn_i_route_rerr_payload {
    uint8_t origin_principal[UCN_I_ROUTE_PRINCIPAL_BYTES];
    uint8_t destination_principal[UCN_I_ROUTE_PRINCIPAL_BYTES];
    uint8_t reporter_principal[UCN_I_ROUTE_PRINCIPAL_BYTES];
    uint64_t route_causal_id;
    uint32_t realm;
    uint32_t origin_address;
    uint32_t origin_binding_generation;
    uint32_t origin_session_generation;
    uint32_t destination_address;
    uint32_t destination_binding_generation;
    uint32_t route_generation;
    uint32_t reporter_address;
    uint32_t reporter_binding_generation;
    uint32_t failed_link_generation;
    uint16_t failed_link_id;
    uint8_t reason;
    uint8_t reserved_zero;
} ucn_i_route_rerr_payload_t;

typedef struct ucn_i_route_rreq_message {
    ucn_i_route_discovery_key_t key;
    ucn_i_route_rreq_payload_t payload;
    uint8_t remaining_hops;
    uint8_t reserved_zero[7];
} ucn_i_route_rreq_message_t;

typedef struct ucn_i_route_rrep_message {
    ucn_i_route_discovery_key_t key;
    ucn_i_route_rrep_payload_t payload;
} ucn_i_route_rrep_message_t;

typedef struct ucn_i_route_soft_view {
    ucn_i_route_domain_t domain;
    ucn_i_route_link_ref_t next_hop;
    uint64_t route_causal_id;
    uint64_t expires_at_us;
    uint32_t runtime_instance;
    uint32_t route_generation;
    uint32_t cost;
    uint16_t owner_instance;
    uint16_t path_frame_mtu;
    uint16_t capability_bits;
    uint8_t hop_count;
    uint8_t reserved_zero;
} ucn_i_route_soft_view_t;

typedef struct ucn_i_route_static_entry {
    ucn_i_route_binding_t destination;
    ucn_i_route_link_ref_t next_hop;
    uint16_t path_frame_mtu;
    uint16_t reserved_zero;
} ucn_i_route_static_entry_t;

typedef struct ucn_i_route_use_facts {
    uint64_t now_us;
    uint32_t origin_session_generation;
    uint32_t destination_binding_generation;
    uint32_t link_generation;
} ucn_i_route_use_facts_t;

typedef uint8_t ucn_i_route_resolved_kind_t;
enum {
    UCN_I_ROUTE_RESOLVED_DYNAMIC = 1,
    UCN_I_ROUTE_RESOLVED_STATIC = 2
};

typedef struct ucn_i_route_resolved {
    ucn_i_route_soft_view_t dynamic;
    ucn_i_route_static_entry_t static_route;
    uint8_t kind;
    uint8_t reserved_zero[7];
} ucn_i_route_resolved_t;

typedef uint8_t ucn_i_route_request_action_kind_t;
enum {
    UCN_I_ROUTE_REQUEST_DUPLICATE = 1,
    UCN_I_ROUTE_REQUEST_LOCAL_TARGET = 2,
    UCN_I_ROUTE_REQUEST_FORWARD = 3
};

typedef struct ucn_i_route_request_action {
    ucn_i_route_rreq_message_t forwarded;
    uint8_t kind;
    uint8_t reserved_zero[7];
} ucn_i_route_request_action_t;

typedef uint8_t ucn_i_route_reply_action_kind_t;
enum {
    UCN_I_ROUTE_REPLY_REACHED_ORIGIN = 1,
    UCN_I_ROUTE_REPLY_FORWARD = 2
};

typedef struct ucn_i_route_reply_action {
    ucn_i_route_rrep_message_t forwarded;
    ucn_i_route_link_ref_t upstream;
    ucn_i_route_soft_view_t installed;
    ucn_handle_t completion;
    uint8_t kind;
    uint8_t reserved_zero[7];
} ucn_i_route_reply_action_t;

typedef struct ucn_i_route_config {
    ucn_i_route_binding_t local;
    uint64_t discovery_lifetime_us;
    uint64_t discovery_retry_us;
    uint64_t reverse_lifetime_us;
    uint64_t route_lifetime_us;
    uint64_t first_transaction_id;
    uint32_t runtime_instance;
    uint32_t realm;
    uint32_t local_session_generation;
    uint16_t owner_instance;
    uint8_t address_width;
    uint8_t discovery_max_attempts;
    uint8_t maximum_hops;
    uint8_t reserved_zero[5];
} ucn_i_route_config_t;

typedef struct ucn_i_route_discovery_record {
    ucn_i_route_rreq_message_t request;
    uint64_t deadline_us;
    uint64_t next_retry_us;
    uint16_t generation;
    uint8_t attempts;
    uint8_t occupied;
} ucn_i_route_discovery_record_t;

typedef struct ucn_i_route_reverse_record {
    ucn_i_route_rreq_message_t accepted_request;
    ucn_i_route_link_ref_t upstream;
    uint64_t expires_at_us;
    uint64_t last_action_us;
    uint16_t generation;
    uint8_t occupied;
    uint8_t reply_pending;
    uint8_t reply_completed;
    uint8_t reserved_zero[3];
} ucn_i_route_reverse_record_t;

typedef struct ucn_i_route_owner {
    uint32_t magic;
    uint32_t runtime_instance;
    uint32_t realm;
    uint32_t local_session_generation;
    uint64_t next_transaction_id;
    uint64_t discovery_lifetime_us;
    uint64_t discovery_retry_us;
    uint64_t reverse_lifetime_us;
    uint64_t route_lifetime_us;
    ucn_i_route_binding_t local;
    ucn_i_lock_ops_t state_lock;
    uint32_t next_route_generation;
    uint16_t schema;
    uint16_t owner_instance;
    uint16_t maintenance_cursor;
    uint8_t address_width;
    uint8_t discovery_max_attempts;
    uint8_t maximum_hops;
    uint8_t reserved_zero;
    ucn_i_route_discovery_record_t discoveries[UCN_I_ROUTE_DISCOVERY_COUNT];
    ucn_i_route_reverse_record_t reverse[UCN_I_ROUTE_REVERSE_COUNT];
    struct {
        ucn_i_route_soft_view_t value;
        uint8_t occupied;
        uint8_t reserved_zero[7];
    } routes[UCN_I_ROUTE_DYNAMIC_COUNT];
    struct {
        ucn_i_route_static_entry_t value;
        uint8_t occupied;
        uint8_t reserved_zero[7];
    } static_routes[UCN_I_ROUTE_STATIC_COUNT];
} ucn_i_route_owner_t;

ucn_result_t ucn_i_route_rreq_encode(const ucn_i_route_rreq_payload_t *value,
                                     uint8_t output[UCN_I_ROUTE_RREQ_BYTES]);
ucn_result_t ucn_i_route_rreq_decode(const uint8_t input[UCN_I_ROUTE_RREQ_BYTES],
                                     ucn_i_route_rreq_payload_t *value_out);
ucn_result_t ucn_i_route_rrep_encode(const ucn_i_route_rrep_payload_t *value,
                                     uint8_t output[UCN_I_ROUTE_RREP_BYTES]);
ucn_result_t ucn_i_route_rrep_decode(const uint8_t input[UCN_I_ROUTE_RREP_BYTES],
                                     ucn_i_route_rrep_payload_t *value_out);
ucn_result_t ucn_i_route_rerr_encode(const ucn_i_route_rerr_payload_t *value,
                                     uint8_t output[UCN_I_ROUTE_RERR_BYTES]);
ucn_result_t ucn_i_route_rerr_decode(const uint8_t input[UCN_I_ROUTE_RERR_BYTES],
                                     ucn_i_route_rerr_payload_t *value_out);

ucn_result_t ucn_i_route_owner_init(ucn_i_route_owner_t *owner,
                                    const ucn_i_route_config_t *config,
                                    const ucn_i_lock_ops_t *state_lock);
ucn_result_t ucn_i_route_owner_destroy(ucn_i_route_owner_t *owner);
ucn_result_t ucn_i_route_install_static(ucn_i_route_owner_t *owner,
                                        const ucn_i_route_static_entry_t *route);
ucn_result_t ucn_i_route_ensure_discovery(ucn_i_route_owner_t *owner,
                                          uint32_t target_address,
                                          uint16_t required_capability_bits,
                                          uint16_t minimum_payload_budget,
                                          uint8_t flags,
                                          uint64_t now_us,
                                          ucn_handle_t *handle_out,
                                          ucn_i_route_rreq_message_t *request_out,
                                          uint8_t *created_out);
ucn_result_t ucn_i_route_retry_discovery(ucn_i_route_owner_t *owner,
                                         ucn_handle_t handle,
                                         uint64_t now_us,
                                         ucn_i_route_rreq_message_t *request_out);
ucn_result_t ucn_i_route_on_rreq(ucn_i_route_owner_t *owner,
                                 const ucn_i_route_rreq_message_t *request,
                                 const ucn_i_route_link_ref_t *ingress,
                                 uint64_t now_us,
                                 ucn_i_route_request_action_t *action_out);
ucn_result_t ucn_i_route_make_rrep(ucn_i_route_owner_t *owner,
                                   const ucn_i_route_rreq_message_t *request,
                                   uint16_t local_capability_bits,
                                   uint16_t local_frame_mtu,
                                   uint64_t now_us,
                                   ucn_i_route_rrep_message_t *reply_out);
ucn_result_t ucn_i_route_on_rrep(ucn_i_route_owner_t *owner,
                                 const ucn_i_route_rrep_message_t *reply,
                                 const ucn_i_route_link_ref_t *ingress,
                                 uint64_t now_us,
                                 ucn_i_route_reply_action_t *action_out);
ucn_result_t ucn_i_route_complete_rrep_forward(ucn_i_route_owner_t *owner,
                                               ucn_handle_t completion,
                                               uint8_t delivered);
ucn_result_t ucn_i_route_resolve(ucn_i_route_owner_t *owner,
                                 const ucn_i_route_domain_t *domain,
                                 const ucn_i_route_use_facts_t *facts,
                                 ucn_i_route_resolved_t *resolved_out);
ucn_result_t ucn_i_route_invalidate_link(ucn_i_route_owner_t *owner,
                                         uint16_t link_id,
                                         uint32_t link_generation,
                                         uint16_t *invalidated_out);
ucn_result_t ucn_i_route_make_rerr(ucn_i_route_owner_t *owner,
                                   const ucn_i_route_soft_view_t *route,
                                   ucn_i_route_rerr_reason_t reason,
                                   ucn_i_route_rerr_payload_t *payload_out);
ucn_result_t ucn_i_route_on_rerr(ucn_i_route_owner_t *owner,
                                 const ucn_i_route_rerr_payload_t *payload,
                                 uint8_t *invalidated_out);
ucn_result_t ucn_i_route_maintain(ucn_i_route_owner_t *owner,
                                  uint64_t now_us,
                                  uint16_t budget,
                                  uint16_t *inspected_out,
                                  uint16_t *expired_out);
ucn_result_t ucn_i_route_counts(ucn_i_route_owner_t *owner,
                                uint16_t *discoveries_out,
                                uint16_t *reverse_out,
                                uint16_t *routes_out,
                                uint16_t *static_out);

#endif
