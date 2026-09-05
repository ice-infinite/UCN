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
#define UCN_V6_PATH_HOP_LIMIT UCN_V6_HOP_COUNT_MAX
#define UCN_V6_CAPABILITY_INVALIDATION_DEPTH                            \
    ((size_t)(UCN_V6_CONFIG_CAPABILITY_PEERS +                          \
              UCN_V6_CONFIG_CAPABILITY_PATHS))

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
    uint32_t ingress_link_generation;
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
    uint32_t destination_session_generation;
    uint32_t destination_capability_generation;
    uint8_t destination_capability_digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    uint16_t destination_realtime_mode_bits;
    uint16_t destination_clock_domain_id;
    uint32_t destination_clock_domain_generation;
    /* EN: Local, immediately revocable parent of this aggregate.  These
     * fields identify the authenticated next Hop seen by this node; they are
     * deliberately independent of the end-to-end destination above.
     * 中文：本机可立即撤销的聚合父级。这些字段标识本节点实际看到的认证
     * 下一跳，刻意与上面的端到端目标身份分离。 */
    ucn_v6_session_key_t local_parent_session;
    uint16_t local_parent_link_id;
    uint32_t local_parent_link_generation;
    uint32_t local_parent_capability_generation;
    uint8_t local_parent_capability_digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    uint32_t route_generation;
    uint16_t path_id;
    uint32_t path_generation;
    uint16_t hop_count;
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

/* EN: Stable lookup keys for Capability Owner consumers.  These references
 * contain identity only; they never carry mutable capability, deadline, MTU,
 * security, or realtime claims.  Consumers must resolve the current value
 * from the Capability Owner immediately before each side effect.
 * 中文：Capability Owner 使用方的稳定查找键。引用只携带身份，不携带可变的
 * 能力、Deadline、MTU、安全或实时声明；使用方必须在每次副作用前从
 * Capability Owner 解析当前值。 */
typedef struct ucn_v6_capability_peer_ref {
    ucn_v6_principal_t principal;
    ucn_v6_binding_key_t binding;
    uint32_t session_generation;
    uint16_t ingress_link_id;
    uint32_t ingress_link_generation;
} ucn_v6_capability_peer_ref_t;

typedef struct ucn_v6_capability_path_ref {
    ucn_v6_principal_t destination_principal;
    ucn_v6_binding_key_t destination_binding;
    uint32_t destination_session_generation;
    uint32_t route_generation;
    uint16_t path_id;
    uint32_t path_generation;
} ucn_v6_capability_path_ref_t;

typedef struct ucn_v6_path_budget_request {
    ucn_v6_principal_t destination_principal;
    ucn_v6_binding_key_t destination_binding;
    uint32_t destination_session_generation;
    uint32_t destination_capability_generation;
    uint8_t destination_capability_digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    uint16_t destination_realtime_mode_bits;
    uint16_t destination_clock_domain_id;
    uint32_t destination_clock_domain_generation;
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
    bool local_parent_bound;
    bool downstream_reduced;
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
              UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS * 32U +               \
              UCN_V6_CAPABILITY_INVALIDATION_DEPTH * 64U))
#endif
typedef union ucn_v6_capability_owner_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_CAPABILITY_OWNER_STORAGE_BYTES];
} ucn_v6_capability_owner_storage_t;

typedef struct ucn_v6_capability_view {
    /* EN: Occupied slots include expired generation history retained until
     * the authenticated Link/Session parent is retired.  Live counts apply
     * the caller-supplied now_us and are safe to use for diagnostics.
     * 中文：占用槽包含过期后保留、直至已认证 Link/Session 父域退休的代际
     * 历史；live 计数使用调用方提供的 now_us，可用于诊断当前活性。 */
    uint16_t occupied_peer_slots;
    uint16_t live_peers;
    uint16_t occupied_path_slots;
    uint16_t live_paths;
    uint16_t group_hints;
    uint16_t pending_invalidations;
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

/* EN: Performs the canonical, read-only liveness validation used by every
 * consumer of a cached peer or derived Path.
 * 中文：执行所有缓存 Peer 与派生 Path 使用方共享的规范只读活性校验。 */
bool ucn_v6_capability_cached_peer_is_live(
    const ucn_v6_cached_peer_capability_t *peer,
    uint64_t now_us);
bool ucn_v6_capability_path_is_live(
    const ucn_v6_path_capability_t *path,
    const ucn_v6_cached_peer_capability_t *destination_peer,
    uint64_t now_us);
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
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_capability_summary_t *summary,
    ucn_v6_hello_disposition_t *disposition);
ucn_v6_result_t ucn_v6_capability_ingest_advertise(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_capability_record_t *record);

/* EN: Converts authenticated Group HELLO only into a bounded reauth hint.
 * 中文：只把认证 Group HELLO 转换为有界重认证提示。 */
ucn_v6_result_t ucn_v6_capability_ingest_group_hello_hint(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_security_open_result_t *opened);

/* EN: Derives a Path in bounded begin/one-Hop/finalize steps.
 * 中文：以 begin/单 Hop/finalize 的有界步骤归约 Path。 */
ucn_v6_result_t ucn_v6_capability_path_reduce_begin(
    const ucn_v6_path_budget_request_t *request,
    ucn_v6_path_budget_accumulator_t *accumulator);
ucn_v6_result_t ucn_v6_capability_path_reduce_hop(
    const ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    ucn_v6_path_budget_accumulator_t *accumulator,
    const ucn_v6_capability_peer_ref_t *hop_ref);
/* EN: Intersects one already-reduced downstream aggregate.  The absolute
 * downstream deadline belongs to another node's clock and is never copied;
 * the caller supplies a locally established authenticated lease deadline.
 * 中文：合并一个已归约的下游聚合。下游绝对 Deadline 属于另一节点时钟，
 * 绝不直接复制；调用方必须提供在本地认证建立的租约截止时间。 */
ucn_v6_result_t ucn_v6_capability_path_reduce_downstream(
    ucn_v6_path_budget_accumulator_t *accumulator,
    const ucn_v6_path_capability_t *downstream,
    uint64_t local_downstream_deadline_us);
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
    uint32_t ingress_link_generation,
    ucn_v6_cached_peer_capability_t *peer);
ucn_v6_result_t ucn_v6_capability_copy_path(
    const ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_principal_t *destination_principal,
    const ucn_v6_binding_key_t *destination_binding,
    uint32_t destination_session_generation,
    uint32_t route_generation,
    uint16_t path_id,
    uint32_t path_generation,
    ucn_v6_path_capability_t *path);
ucn_v6_result_t ucn_v6_capability_expire(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us);

/* EN: Applies a canonical parent invalidation to the capability cache.
 * Capability lease expiry revokes liveness but deliberately retains the
 * generation/digest history until the authenticated Link or Session parent is
 * invalidated.  This prevents an expired peer from reopening the same parent
 * domain with an older capability generation while still allowing bounded
 * slot reclamation when that parent is retired.
 * 中文：把规范父级失效事件应用到能力缓存。能力租约到期只撤销活性，仍保留
 * generation/digest 历史，直至已认证 Link 或 Session 父域明确失效；这样既
 * 防止同一父域用旧能力代际重新进入，也能在父域退休后有界回收槽位。 */
ucn_v6_result_t ucn_v6_capability_apply_invalidation(
    ucn_v6_capability_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation);

/* EN: Peeks and exactly acknowledges bounded dependency invalidations.
 * Rejected peeks/acks never mutate the queue or output.
 * 中文：非破坏性查看并精确确认有界依赖失效事件；拒绝路径不修改队列或输出。 */
ucn_v6_result_t ucn_v6_capability_invalidation_peek(
    const ucn_v6_capability_owner_t *owner,
    ucn_v6_stack_invalidation_t *invalidation);
ucn_v6_result_t ucn_v6_capability_invalidation_ack(
    ucn_v6_capability_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation);
ucn_v6_result_t ucn_v6_capability_copy_view(
    const ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    ucn_v6_capability_view_t *view);

#ifdef __cplusplus
}
#endif

#endif
