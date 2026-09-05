#ifndef UCN_V6_CAPABILITY_H
#define UCN_V6_CAPABILITY_H

#include "ucn/v6/ucn_v6_security.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_CAPABILITY_DIGEST_BYTES ((size_t)16U)
#define UCN_V6_CAPABILITY_RECORD_BYTES ((size_t)68U)
#define UCN_V6_CAPABILITY_HELLO_BYTES ((size_t)24U)
#define UCN_V6_CAPABILITY_QUERY_BYTES ((size_t)20U)
#define UCN_V6_GROUP_HINT_TIMEOUT_US UINT64_C(1000000)
#define UCN_V6_GROUP_HINTS_PER_LINK ((uint8_t)2U)
#define UCN_V6_PATH_HOP_LIMIT UINT16_C(65534)

enum {
    UCN_V6_LINK_ORDERED = 1U << 0,
    UCN_V6_LINK_RELIABLE = 1U << 1,
    UCN_V6_LINK_BROADCAST = 1U << 2,
    UCN_V6_LINK_UNICAST = 1U << 3,
    UCN_V6_LINK_SECURITY = 1U << 4
};

enum {
    UCN_V6_REALTIME_MODE_LOCAL = 1U << 0,
    UCN_V6_REALTIME_MODE_SYNCED = 1U << 1,
    UCN_V6_REALTIME_MODE_DEADLINE = 1U << 2
};

enum {
    UCN_V6_TIMESTAMP_RX_SOFTWARE = 1U << 0,
    UCN_V6_TIMESTAMP_TX_SOFTWARE = 1U << 1,
    UCN_V6_TIMESTAMP_RX_HARDWARE = 1U << 2,
    UCN_V6_TIMESTAMP_TX_HARDWARE = 1U << 3
};

typedef enum ucn_v6_message_class {
    UCN_V6_MESSAGE_T32 = 0,
    UCN_V6_MESSAGE_T64 = 1,
    UCN_V6_MESSAGE_T128 = 2,
    UCN_V6_MESSAGE_T256 = 3,
    UCN_V6_MESSAGE_T512 = 4,
    UCN_V6_MESSAGE_T1K = 5,
    UCN_V6_MESSAGE_T2K = 6,
    UCN_V6_MESSAGE_T4K = 7,
    UCN_V6_MESSAGE_T8K = 8
} ucn_v6_message_class_t;

typedef struct ucn_v6_link_capability {
    uint32_t link_instance_generation;
    uint32_t carrier_mtu;
    uint32_t link_frame_mtu;
    uint32_t processing_frame_mtu;
    uint16_t carrier_header_bytes;
    uint16_t carrier_padding_bytes;
    uint16_t carrier_crc_bytes;
    uint16_t carrier_tag_bytes;
    uint16_t carrier_max_fragments;
    uint16_t link_flags;
    uint32_t nominal_rate_bps;
    uint8_t hardware_priority_count;
    uint16_t timestamp_capability_bits;
    uint32_t timestamp_uncertainty_us;
} ucn_v6_link_capability_t;

typedef struct ucn_v6_peer_capability {
    uint32_t feature_bits;
    uint32_t hop_suite_bits;
    uint32_t e2e_suite_bits;
    ucn_v6_message_class_t max_message_class;
    uint16_t max_rx_window;
    uint16_t max_concurrent_transfers;
    uint16_t realtime_mode_bits;
    uint16_t clock_domain_id;
    uint32_t clock_domain_generation;
} ucn_v6_peer_capability_t;

typedef struct ucn_v6_capability_record {
    uint32_t capability_generation;
    ucn_v6_link_capability_t link;
    ucn_v6_peer_capability_t peer;
} ucn_v6_capability_record_t;

typedef struct ucn_v6_capability_summary {
    uint32_t capability_generation;
    uint32_t link_instance_generation;
    uint8_t digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
} ucn_v6_capability_summary_t;

typedef struct ucn_v6_capability_query {
    uint32_t requested_generation;
    uint8_t known_digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
} ucn_v6_capability_query_t;

typedef enum ucn_v6_hello_disposition {
    UCN_V6_HELLO_MATCHED = 1,
    UCN_V6_HELLO_QUERY_REQUIRED = 2
} ucn_v6_hello_disposition_t;

typedef struct ucn_v6_cached_peer_capability {
    bool valid;
    ucn_v6_principal_t principal;
    ucn_v6_binding_key_t binding;
    uint32_t session_generation;
    uint16_t ingress_link_id;
    ucn_v6_capability_record_t record;
    uint8_t digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    uint64_t discovery_deadline_us;
    uint64_t capability_deadline_us;
} ucn_v6_cached_peer_capability_t;

typedef struct ucn_v6_path_capability {
    bool valid;
    bool immutable_for_realtime;
    ucn_v6_principal_t destination_principal;
    ucn_v6_binding_key_t destination_binding;
    uint32_t session_generation;
    uint32_t destination_link_instance_generation;
    uint8_t destination_capability_digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    uint32_t route_generation;
    uint16_t path_id;
    uint32_t path_generation;
    uint32_t path_frame_mtu;
    uint32_t payload_budget;
    uint32_t fragment_data_budget;
    uint32_t feature_bits;
    uint32_t hop_suite_bits;
    uint32_t e2e_suite_bits;
    ucn_v6_message_class_t max_message_class;
    uint16_t max_window;
    uint16_t max_concurrency;
    uint16_t timestamp_capability_bits;
    uint32_t timestamp_uncertainty_us;
    uint64_t deadline_us;
} ucn_v6_path_capability_t;

typedef struct ucn_v6_path_budget_request {
    ucn_v6_principal_t destination_principal;
    ucn_v6_binding_key_t destination_binding;
    uint32_t session_generation;
    uint32_t destination_link_instance_generation;
    uint8_t destination_capability_digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    uint32_t route_generation;
    uint16_t path_id;
    uint32_t path_generation;
    uint64_t deadline_us;
    bool fixed_path;
    uint32_t path_policy_frame_mtu;
    uint32_t required_feature_bits;
    uint32_t required_hop_suite_bits;
    uint32_t required_e2e_suite_bits;
    ucn_v6_message_class_t policy_max_message_class;
    uint16_t policy_max_window;
    uint16_t policy_max_concurrency;
    const ucn_v6_frame_t *frame_contract;
    uint16_t fragment_header_bytes;
} ucn_v6_path_budget_request_t;

/* Stack/caller-owned streaming reducer.  Each accumulate call performs
 * constant work for exactly one Hop, so Protocol Owner budgets stay bounded
 * without imposing a small network-wide hop limit. */
typedef struct ucn_v6_path_budget_accumulator {
    ucn_v6_path_capability_t derived;
    uint32_t required_feature_bits;
    uint32_t required_hop_suite_bits;
    uint32_t required_e2e_suite_bits;
    uint32_t frame_overhead_bytes;
    uint64_t timestamp_uncertainty_us;
    uint16_t fragment_header_bytes;
    uint16_t hop_count;
    bool active;
} ucn_v6_path_budget_accumulator_t;

typedef struct ucn_v6_group_discovery_hint {
    bool occupied;
    uint16_t ingress_link_id;
    uint32_t ingress_link_generation;
    uint32_t group_id;
    uint32_t group_generation;
    ucn_v6_binding_key_t claimed_binding;
    uint32_t claimed_session_generation;
    uint64_t deadline_us;
} ucn_v6_group_discovery_hint_t;

typedef struct ucn_v6_capability_owner ucn_v6_capability_owner_t;
#ifndef UCN_V6_CAPABILITY_OWNER_STORAGE_BYTES
#define UCN_V6_CAPABILITY_OWNER_STORAGE_BYTES                            \
    ((size_t)(1024U + UCN_V6_CONFIG_CAPABILITY_PEERS * 256U +           \
              UCN_V6_CONFIG_CAPABILITY_PATHS * 160U +                   \
              UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS * 136U +              \
              UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS * 32U))
#endif
typedef union ucn_v6_capability_owner_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_CAPABILITY_OWNER_STORAGE_BYTES];
} ucn_v6_capability_owner_storage_t;

typedef struct ucn_v6_capability_view {
    uint16_t cached_peers;
    uint16_t installed_paths;
    uint16_t group_hints;
    uint32_t rejected_authentication;
    uint32_t rejected_capacity;
    bool faulted;
} ucn_v6_capability_view_t;

/* EN: Encodes/decodes the exact authenticated capability payloads.
 * 中文：编码/解码精确定义的认证能力载荷。 */
ucn_v6_result_t ucn_v6_capability_record_encode(
    const ucn_v6_capability_record_t *record,
    uint8_t output[UCN_V6_CAPABILITY_RECORD_BYTES]);
ucn_v6_result_t ucn_v6_capability_record_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_capability_record_t *record);
ucn_v6_result_t ucn_v6_capability_digest(
    const ucn_v6_capability_record_t *record,
    uint8_t digest[UCN_V6_CAPABILITY_DIGEST_BYTES]);
ucn_v6_result_t ucn_v6_capability_summary_encode(
    const ucn_v6_capability_summary_t *summary,
    uint8_t output[UCN_V6_CAPABILITY_HELLO_BYTES]);
ucn_v6_result_t ucn_v6_capability_summary_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_capability_summary_t *summary);
ucn_v6_result_t ucn_v6_capability_query_encode(
    const ucn_v6_capability_query_t *query,
    uint8_t output[UCN_V6_CAPABILITY_QUERY_BYTES]);
ucn_v6_result_t ucn_v6_capability_query_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_capability_query_t *query);

/* EN: Initializes the isolated fixed-capacity capability owner.
 * 中文：初始化隔离、固定容量的能力 Owner。 */
ucn_v6_result_t ucn_v6_capability_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_capability_record_t *local_record,
    uint64_t local_capability_lease_us,
    uint64_t discovery_lease_us,
    ucn_v6_capability_owner_t **owner);

/* EN: Handles authenticated Peer HELLO/ADVERTISE without granting authority.
 * 中文：处理已认证 Peer HELLO/ADVERTISE，但不授予任何 Authority。 */
ucn_v6_result_t ucn_v6_capability_ingest_peer_hello(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    uint16_t ingress_link_id,
    uint32_t ingress_link_generation,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_capability_summary_t *summary,
    ucn_v6_hello_disposition_t *disposition);
ucn_v6_result_t ucn_v6_capability_ingest_advertise(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    uint16_t ingress_link_id,
    uint32_t ingress_link_generation,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_capability_record_t *record);

/* EN: Converts authenticated Group HELLO only into a bounded reauth hint.
 * 中文：只把认证 Group HELLO 转换为有界重认证提示。 */
ucn_v6_result_t ucn_v6_capability_ingest_group_hello_hint(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    uint16_t ingress_link_id,
    uint32_t ingress_link_generation,
    const ucn_v6_security_open_result_t *opened);

/* EN: Derives a Path in bounded begin/one-Hop/finalize steps.
 * 中文：以 begin/单 Hop/finalize 的有界步骤归约 Path。 */
ucn_v6_result_t ucn_v6_capability_path_reduce_begin(
    const ucn_v6_path_budget_request_t *request,
    ucn_v6_path_budget_accumulator_t *accumulator);
ucn_v6_result_t ucn_v6_capability_path_reduce_hop(
    ucn_v6_path_budget_accumulator_t *accumulator,
    const ucn_v6_capability_record_t *hop);
ucn_v6_result_t ucn_v6_capability_path_reduce_finalize(
    ucn_v6_path_budget_accumulator_t *accumulator,
    ucn_v6_path_capability_t *path);
ucn_v6_result_t ucn_v6_capability_install_path(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_path_capability_t *path);
ucn_v6_result_t ucn_v6_capability_copy_peer(
    const ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_principal_t *principal,
    const ucn_v6_binding_key_t *binding,
    uint32_t session_generation,
    uint32_t link_instance_generation,
    ucn_v6_cached_peer_capability_t *peer);
ucn_v6_result_t ucn_v6_capability_copy_path(
    const ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_principal_t *destination_principal,
    const ucn_v6_binding_key_t *destination_binding,
    uint32_t session_generation,
    uint32_t route_generation,
    uint16_t path_id,
    uint32_t path_generation,
    ucn_v6_path_capability_t *path);
ucn_v6_result_t ucn_v6_capability_expire(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us);
ucn_v6_result_t ucn_v6_capability_copy_view(
    const ucn_v6_capability_owner_t *owner,
    ucn_v6_capability_view_t *view);

#ifdef __cplusplus
}
#endif

#endif
