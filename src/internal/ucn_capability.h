#ifndef UCN_INTERNAL_CAPABILITY_H
#define UCN_INTERNAL_CAPABILITY_H

#include "internal/ucn_owner.h"
#include "ucn/ucn_config.h"

#define UCN_I_CAPABILITY_SCHEMA UINT16_C(1)
#define UCN_I_CAPABILITY_RECORD_BYTES 68U
#define UCN_I_CAPABILITY_SUMMARY_BYTES 24U
#define UCN_I_CAPABILITY_QUERY_BYTES 20U
#define UCN_I_CAPABILITY_DIGEST_BYTES 16U
#define UCN_I_CAPABILITY_PRINCIPAL_BYTES 16U
#define UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES UINT32_C(0x0000010B)
#define UCN_I_CAPABILITY_KNOWN_FEATURES UINT32_C(0x000007FF)
#define UCN_I_CAPABILITY_KNOWN_HOP_SUITES UINT32_C(0x00000002)
#define UCN_I_CAPABILITY_KNOWN_E2E_SUITES UINT32_C(0x0000000E)

#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_I_CAPABILITY_PEER_COUNT 4U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_I_CAPABILITY_PEER_COUNT 8U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define UCN_I_CAPABILITY_PEER_COUNT 16U
#else
#error "UCN_PROFILE must be UCN_PROFILE_NANO, UCN_PROFILE_LITE, or UCN_PROFILE_FULL"
#endif

typedef uint8_t ucn_i_message_class_t;
enum {
    UCN_I_MESSAGE_T32 = 0,
    UCN_I_MESSAGE_T64 = 1,
    UCN_I_MESSAGE_T128 = 2,
    UCN_I_MESSAGE_T256 = 3,
    UCN_I_MESSAGE_T512 = 4,
    UCN_I_MESSAGE_T1K = 5,
    UCN_I_MESSAGE_T2K = 6,
    UCN_I_MESSAGE_T4K = 7,
    UCN_I_MESSAGE_T8K = 8
};

typedef struct ucn_i_link_capability {
    uint32_t link_instance_generation;
    uint32_t carrier_mtu;
    uint32_t link_frame_mtu;
    uint32_t processing_frame_mtu;
    uint32_t nominal_rate_bps;
    uint32_t timestamp_uncertainty_us;
    uint16_t carrier_header_bytes;
    uint16_t carrier_padding_bytes;
    uint16_t carrier_crc_bytes;
    uint16_t carrier_tag_bytes;
    uint16_t carrier_max_fragments;
    uint16_t link_flags;
    uint16_t timestamp_capability_bits;
    uint8_t hardware_priority_count;
    uint8_t reserved_zero;
} ucn_i_link_capability_t;

typedef struct ucn_i_peer_capability {
    uint32_t feature_bits;
    uint32_t hop_suite_bits;
    uint32_t e2e_suite_bits;
    uint32_t clock_domain_generation;
    uint16_t max_rx_window;
    uint16_t max_concurrent_transfers;
    uint16_t realtime_mode_bits;
    uint16_t clock_domain_id;
    uint8_t max_message_class;
    uint8_t reserved_zero[3];
} ucn_i_peer_capability_t;

typedef struct ucn_i_capability_record {
    uint32_t capability_generation;
    ucn_i_link_capability_t link;
    ucn_i_peer_capability_t peer;
} ucn_i_capability_record_t;

typedef struct ucn_i_capability_summary {
    uint32_t capability_generation;
    uint32_t link_instance_generation;
    uint8_t digest[UCN_I_CAPABILITY_DIGEST_BYTES];
} ucn_i_capability_summary_t;

typedef struct ucn_i_capability_query {
    uint32_t requested_generation;
    uint8_t known_digest[UCN_I_CAPABILITY_DIGEST_BYTES];
} ucn_i_capability_query_t;

/* EN: Coordinator obtains this immutable, authenticated projection from the
 * Security Owner. Capability never receives a Security Owner pointer.
 * 中文：Coordinator 从 Security Owner 取得该不可变认证投影。Capability
 * 永远不持有 Security Owner 指针。 */
typedef struct ucn_i_authenticated_peer_view {
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint32_t address;
    uint32_t binding_generation;
    uint32_t session_generation;
    uint32_t link_id;
    uint32_t link_generation;
    uint64_t expires_at_us;
    uint16_t security_owner_instance;
    uint16_t reserved_zero;
    uint8_t principal[UCN_I_CAPABILITY_PRINCIPAL_BYTES];
} ucn_i_authenticated_peer_view_t;

typedef struct ucn_i_capability_ref {
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint32_t address;
    uint32_t binding_generation;
    uint32_t session_generation;
    uint32_t link_id;
    uint32_t link_generation;
    uint16_t security_owner_instance;
    uint16_t reserved_zero;
    uint8_t principal[UCN_I_CAPABILITY_PRINCIPAL_BYTES];
} ucn_i_capability_ref_t;

typedef struct ucn_i_cached_capability {
    ucn_i_capability_ref_t reference;
    ucn_i_capability_record_t record;
    uint64_t discovery_deadline_us;
    uint64_t capability_deadline_us;
    uint8_t digest[UCN_I_CAPABILITY_DIGEST_BYTES];
} ucn_i_cached_capability_t;

typedef uint8_t ucn_i_capability_summary_result_t;
enum {
    UCN_I_CAPABILITY_SUMMARY_MATCHED = 1,
    UCN_I_CAPABILITY_SUMMARY_QUERY_REQUIRED = 2
};

typedef struct ucn_i_capability_owner {
    uint32_t magic;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint16_t schema;
    uint16_t owner_instance;
    uint16_t security_owner_instance;
    uint16_t maintenance_cursor;
    uint64_t discovery_lease_us;
    uint64_t capability_lease_us;
    ucn_i_lock_ops_t state_lock;
    struct {
        ucn_i_cached_capability_t value;
        uint8_t occupied;
        uint8_t reserved_zero[7];
    } peers[UCN_I_CAPABILITY_PEER_COUNT];
} ucn_i_capability_owner_t;

typedef struct ucn_i_profile_requirements {
    uint32_t required_feature_bits;
    uint32_t required_hop_suite_bits;
    uint32_t required_e2e_suite_bits;
    uint16_t minimum_rx_window;
    uint16_t minimum_concurrent_transfers;
    uint16_t required_realtime_mode_bits;
    uint8_t minimum_message_class;
    uint8_t reserved_zero;
} ucn_i_profile_requirements_t;

typedef struct ucn_i_profile_select {
    uint32_t local_generation;
    uint32_t peer_generation;
    uint32_t feature_bits;
    uint32_t hop_suite_bits;
    uint32_t e2e_suite_bits;
    uint16_t max_rx_window;
    uint16_t max_concurrent_transfers;
    uint16_t realtime_mode_bits;
    uint8_t max_message_class;
    uint8_t reserved_zero;
    uint8_t local_digest[16];
    uint8_t peer_digest[16];
} ucn_i_profile_select_t;

typedef struct ucn_i_profile_ack {
    ucn_i_profile_select_t selected;
    uint8_t transcript_digest[16];
} ucn_i_profile_ack_t;

/* EN: Resolver dependency choices are not Coordinator routing kinds.  Keep
 * the namespaces distinct so a production composition may include both
 * contracts without relying on include order.
 * 中文：Resolver 的依赖选择并不是 Coordinator 的路由类型。二者必须使用独立
 * 命名域，使生产装配可同时包含两份合同而不依赖头文件顺序。 */
typedef uint8_t ucn_i_resolve_dependency_kind_t;
enum {
    UCN_I_DEPENDENCY_IDENTITY = 1,
    UCN_I_DEPENDENCY_SECURITY = 2,
    UCN_I_DEPENDENCY_CAPABILITY = 3,
    UCN_I_DEPENDENCY_ROUTE = 4,
    UCN_I_DEPENDENCY_FLOW = 5,
    UCN_I_DEPENDENCY_TIME = 6,
    UCN_I_DEPENDENCY_TRANSFER = 7
};

typedef struct ucn_i_effective_intent {
    uint32_t required_feature_bits;
    uint32_t forbidden_feature_bits;
    uint32_t payload_bytes;
    uint8_t security_floor;
    uint8_t reliable_required;
    uint8_t realtime_required;
    uint8_t pinned_path_required;
} ucn_i_effective_intent_t;

typedef struct ucn_i_resource_view {
    uint8_t tx_available;
    uint8_t reliable_available;
    uint8_t transfer_available;
    uint8_t reserved_zero;
} ucn_i_resource_view_t;

typedef struct ucn_i_contract_candidate {
    uint32_t feature_bits;
    uint32_t payload_budget;
    uint32_t exact_frame_bytes;
    uint32_t setup_cost_bytes;
    uint16_t expected_reuse_count;
    uint16_t stable_order;
    uint8_t contract;
    uint8_t origin_security;
    uint8_t hop_profile;
    uint8_t reliable;
    uint8_t realtime;
    uint8_t pinned_path;
    uint8_t missing_dependency;
    uint8_t reserved_zero;
} ucn_i_contract_candidate_t;

typedef uint8_t ucn_i_resolve_status_t;
enum {
    UCN_I_RESOLVE_READY = 1,
    UCN_I_RESOLVE_NEED_DEPENDENCY = 2,
    UCN_I_RESOLVE_REJECT_UNSUPPORTED = 3,
    UCN_I_RESOLVE_REJECT_POLICY = 4,
    UCN_I_RESOLVE_REJECT_RESOURCE = 5
};

typedef struct ucn_i_resolve_result {
    ucn_i_contract_candidate_t candidate;
    uint8_t status;
    uint8_t dependency;
    uint8_t reserved_zero[2];
} ucn_i_resolve_result_t;

ucn_result_t ucn_i_capability_record_encode(
    const ucn_i_capability_record_t *record,
    uint8_t output[UCN_I_CAPABILITY_RECORD_BYTES]);
ucn_result_t ucn_i_capability_record_decode(
    const uint8_t input[UCN_I_CAPABILITY_RECORD_BYTES],
    ucn_i_capability_record_t *record_out);
ucn_result_t ucn_i_capability_digest(
    const ucn_i_capability_record_t *record,
    uint8_t digest_out[UCN_I_CAPABILITY_DIGEST_BYTES]);
ucn_result_t ucn_i_capability_summary_encode(
    const ucn_i_capability_summary_t *summary,
    uint8_t output[UCN_I_CAPABILITY_SUMMARY_BYTES]);
ucn_result_t ucn_i_capability_summary_decode(
    const uint8_t input[UCN_I_CAPABILITY_SUMMARY_BYTES],
    ucn_i_capability_summary_t *summary_out);
ucn_result_t ucn_i_capability_query_encode(
    const ucn_i_capability_query_t *query,
    uint8_t output[UCN_I_CAPABILITY_QUERY_BYTES]);
ucn_result_t ucn_i_capability_query_decode(
    const uint8_t input[UCN_I_CAPABILITY_QUERY_BYTES],
    ucn_i_capability_query_t *query_out);
ucn_result_t ucn_i_capability_owner_init(
    ucn_i_capability_owner_t *owner,
    uint32_t runtime_instance,
    uint32_t realm_id,
    uint16_t owner_instance,
    uint16_t security_owner_instance,
    uint64_t discovery_lease_us,
    uint64_t capability_lease_us,
    const ucn_i_lock_ops_t *state_lock);
ucn_result_t ucn_i_capability_summary_ingest(
    ucn_i_capability_owner_t *owner,
    const ucn_i_authenticated_peer_view_t *authenticated,
    const ucn_i_capability_summary_t *summary,
    uint64_t now_us,
    ucn_i_capability_summary_result_t *result_out);
ucn_result_t ucn_i_capability_advertise_ingest(
    ucn_i_capability_owner_t *owner,
    const ucn_i_authenticated_peer_view_t *authenticated,
    const ucn_i_capability_record_t *record,
    uint64_t now_us,
    ucn_i_capability_ref_t *reference_out);
ucn_result_t ucn_i_capability_get(
    ucn_i_capability_owner_t *owner,
    const ucn_i_capability_ref_t *reference,
    uint64_t now_us,
    ucn_i_cached_capability_t *value_out);
ucn_result_t ucn_i_capability_invalidate_session(
    ucn_i_capability_owner_t *owner,
    const ucn_i_capability_ref_t *reference);
ucn_result_t ucn_i_capability_maintain(
    ucn_i_capability_owner_t *owner,
    uint64_t now_us,
    uint16_t budget,
    ucn_i_capability_ref_t *expired_reference_out,
    uint16_t *inspected_out,
    uint8_t *expired_valid_out);
ucn_result_t ucn_i_capability_owner_destroy(
    ucn_i_capability_owner_t *owner);
ucn_result_t ucn_i_profile_select_build(
    const ucn_i_capability_record_t *local,
    const uint8_t local_digest[16],
    const ucn_i_capability_record_t *peer,
    const uint8_t peer_digest[16],
    const ucn_i_profile_requirements_t *requirements,
    ucn_i_profile_select_t *select_out);
ucn_result_t ucn_i_profile_ack_verify_exact(
    const ucn_i_profile_ack_t *ack,
    const ucn_i_profile_select_t *expected,
    const uint8_t expected_transcript_digest[16]);
ucn_result_t ucn_i_contract_resolve(
    const ucn_i_effective_intent_t *intent,
    const ucn_i_resource_view_t *resources,
    const ucn_i_contract_candidate_t *candidates,
    size_t candidate_count,
    ucn_i_resolve_result_t *result_out);

#endif
