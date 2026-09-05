#ifndef UCN_V6_TRANSFER_H
#define UCN_V6_TRANSFER_H

#include "ucn/v6/ucn_v6_qos.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_TRANSFER_FRAGMENT_HEADER_BYTES ((size_t)24U)
#define UCN_V6_TRANSFER_SACK_BYTES ((size_t)20U)
#define UCN_V6_TRANSFER_CREDIT_BYTES ((size_t)32U)
#define UCN_V6_TRANSFER_RESULT_BYTES ((size_t)24U)
#define UCN_V6_TRANSFER_MAX_WINDOW ((uint16_t)32U)

typedef enum ucn_v6_transfer_payload_type {
    UCN_V6_TRANSFER_PAYLOAD_FRAGMENT = 1,
    UCN_V6_TRANSFER_PAYLOAD_SACK = 2,
    UCN_V6_TRANSFER_PAYLOAD_CREDIT = 3,
    UCN_V6_TRANSFER_PAYLOAD_RESULT = 4
} ucn_v6_transfer_payload_type_t;

typedef struct ucn_v6_transfer_fragment {
    ucn_v6_message_class_t message_class;
    uint64_t message_id;
    uint16_t total_length;
    uint16_t fragment_index;
    uint16_t fragment_count;
    uint16_t fragment_data_budget;
    uint16_t data_length;
    uint32_t message_crc32c;
    const uint8_t *data;
} ucn_v6_transfer_fragment_t;

typedef struct ucn_v6_transfer_sack {
    uint64_t message_id;
    uint16_t cumulative_base;
    uint16_t fragment_count;
    uint32_t received_bitmap;
} ucn_v6_transfer_sack_t;

typedef struct ucn_v6_transfer_credit_update {
    uint16_t link_id;
    uint32_t link_generation;
    ucn_v6_traffic_class_t traffic_class;
    uint32_t credit_generation;
    uint32_t update_sequence;
    uint16_t available_credit;
    uint16_t maximum_credit;
    uint64_t lease_duration_us;
} ucn_v6_transfer_credit_update_t;

typedef struct ucn_v6_transfer_result {
    uint64_t message_id;
    uint64_t operation_id;
    int32_t result_code;
} ucn_v6_transfer_result_t;

typedef struct ucn_v6_transfer_send_request {
    /* EN: Stable identity only. Transfer resolves and freezes the exact live
     * Route path from its init-bound Route Owner; callers cannot inject an
     * egress, next-hop or Capability claim.
     * 中文：这里只携带稳定身份。Transfer 从初始化时绑定的 Route Owner
     * 解析并冻结精确活跃路径，调用方不能注入出口、下一跳或能力声明。 */
    ucn_v6_route_path_ref_t route_ref;
    ucn_v6_message_descriptor_t message;
    ucn_v6_message_class_t message_class;
    uint64_t message_id;
    uint64_t buffer_token;
    const uint8_t *payload;
    uint16_t payload_length;
    uint16_t fragment_data_budget;
    uint16_t window_size;
} ucn_v6_transfer_send_request_t;

/* EN: message_id is the request operation_id, but Transfer does not allocate
 * or impose a global contiguous order on that identifier. Durable duplicate
 * execution protection belongs to the Message operation journal and is
 * scoped by the authenticated initiator. The caller owns payload and must
 * keep it immutable until retire_tx returns its buffer_token.
 * 中文：message_id 等于请求的 operation_id，但 Transfer 不分配该
 * 标识，也不强制全局连续顺序。持久化的防重复执行由 Message
 * Operation Journal 按认证发起者域负责。payload 由调用方持有，
 * 并须保持不变，直到 retire_tx 返还 buffer_token。 */

typedef enum ucn_v6_transfer_tx_phase {
    UCN_V6_TRANSFER_TX_SENDING = 1,
    UCN_V6_TRANSFER_TX_REASSEMBLED = 2,
    UCN_V6_TRANSFER_TX_FAILED = 3
} ucn_v6_transfer_tx_phase_t;

typedef struct ucn_v6_transfer_tx_view {
    uint64_t message_id;
    uint64_t buffer_token;
    ucn_v6_transfer_tx_phase_t phase;
    uint16_t fragment_count;
    uint16_t cumulative_base;
    uint16_t fragment_data_budget;
    uint16_t window_size;
} ucn_v6_transfer_tx_view_t;

typedef struct ucn_v6_transfer_rx_result {
    bool accepted;
    bool complete;
    bool recent_replay;
    ucn_v6_transfer_sack_t sack;
} ucn_v6_transfer_rx_result_t;

typedef struct ucn_v6_transfer_completed {
    ucn_v6_session_key_t origin;
    /* EN: Exact end-to-end business semantics frozen by the first accepted
     * fragment.  The receiver uses this descriptor to dispatch the
     * reassembled bytes; later fragments cannot change its endpoints, role,
     * traffic class, delivery guarantee or operation identity.
     * 中文：首个已接受分片冻结的完整端到端业务语义。接收端使用该描述符
     * 分发重组后的字节；后续分片不得改变端点、角色、流量等级、交付保证
     * 或操作身份。 */
    ucn_v6_message_descriptor_t message;
    uint64_t operation_id;
    uint64_t message_id;
    ucn_v6_message_class_t message_class;
    uint16_t payload_length;
    uint32_t message_crc32c;
} ucn_v6_transfer_completed_t;

typedef struct ucn_v6_transfer_credit_reservation {
    uint64_t reservation_id;
    ucn_v6_session_key_t peer;
    uint16_t link_id;
    uint32_t link_generation;
    ucn_v6_traffic_class_t traffic_class;
    uint32_t credit_generation;
} ucn_v6_transfer_credit_reservation_t;

typedef struct ucn_v6_transfer_stats {
    uint32_t transfers_started;
    uint32_t fragments_submitted;
    uint32_t fragments_retransmitted;
    uint32_t sacks_applied;
    uint32_t receive_fragments;
    uint32_t receive_duplicates;
    uint32_t completed_messages;
    uint32_t credit_rejections;
    uint16_t tx_active;
    uint16_t rx_active;
    uint16_t recent_completions;
    uint16_t credit_reservations;
    bool selection_pending;
    bool faulted;
} ucn_v6_transfer_stats_t;

typedef struct ucn_v6_transfer_invalidation_result {
    uint16_t tx_retired;
    uint16_t rx_retired;
    uint16_t recent_retired;
    uint16_t credit_slots_retired;
    uint16_t credit_reservations_retired;
} ucn_v6_transfer_invalidation_result_t;

typedef struct ucn_v6_transfer_owner ucn_v6_transfer_owner_t;
typedef union ucn_v6_transfer_owner_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_TRANSFER_OWNER_STORAGE_BYTES];
} ucn_v6_transfer_owner_storage_t;

size_t ucn_v6_message_class_bytes(ucn_v6_message_class_t message_class);
/* EN: Fragment input and encoded output must not overlap.
 * 中文：分片输入数据与编码输出缓冲区不得重叠。 */
ucn_v6_result_t ucn_v6_transfer_fragment_encode(
    const ucn_v6_transfer_fragment_t *fragment,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);
ucn_v6_result_t ucn_v6_transfer_fragment_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_transfer_fragment_t *fragment);
ucn_v6_result_t ucn_v6_transfer_sack_encode(
    const ucn_v6_transfer_sack_t *sack,
    uint8_t output[UCN_V6_TRANSFER_SACK_BYTES]);
ucn_v6_result_t ucn_v6_transfer_sack_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_transfer_sack_t *sack);
ucn_v6_result_t ucn_v6_transfer_credit_encode(
    const ucn_v6_transfer_credit_update_t *credit,
    uint8_t output[UCN_V6_TRANSFER_CREDIT_BYTES]);
ucn_v6_result_t ucn_v6_transfer_credit_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_transfer_credit_update_t *credit);
ucn_v6_result_t ucn_v6_transfer_result_encode(
    const ucn_v6_transfer_result_t *result,
    uint8_t output[UCN_V6_TRANSFER_RESULT_BYTES]);
ucn_v6_result_t ucn_v6_transfer_result_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_transfer_result_t *result);

/* EN: A non-NULL Route Owner is permanently bound and must outlive Transfer.
 * It is required for TX but may be NULL for an RX-only owner. Every TX
 * selection, submit, SACK, expiry and proactive rebind resolves the exact
 * frozen Route path again.  The one canonical dependency chain includes the
 * local egress Link/Session/Capability and Path identity; the destination
 * capability claim is revalidated by Route Owner. Cleanup remains possible
 * after revocation so a caller-owned buffer token cannot be stranded.
 * 中文：非空 Route Owner 在初始化时永久绑定，且生命周期必须长于 Transfer；
 * TX 必须提供，纯 RX Owner 可传 NULL。每次 TX 选择、提交、SACK、过期检查和
 * 主动换路都会重新解析完全一致的 Route Path。唯一规范依赖链包含本机出口
 * Link/Session/Capability 与 Path 身份，目标能力声明由 Route Owner 重验；
 * 撤权后仍允许清理，避免调用方 Buffer Token 被永久滞留。 */
ucn_v6_result_t ucn_v6_transfer_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_route_owner_t *route_owner,
    uint64_t retry_interval_us,
    uint8_t fragment_max_attempts,
    uint64_t receive_timeout_us,
    uint64_t recent_completion_us,
    ucn_v6_transfer_owner_t **owner);
ucn_v6_result_t ucn_v6_transfer_send_begin(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_transfer_send_request_t *request);
ucn_v6_result_t ucn_v6_transfer_next_fragment(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    uint64_t message_id,
    ucn_v6_transfer_fragment_t *fragment);
ucn_v6_result_t ucn_v6_transfer_record_fragment_submit(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    uint64_t message_id,
    uint16_t fragment_index,
    bool submitted);
ucn_v6_result_t ucn_v6_transfer_apply_sack(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_security_open_result_t *opened);
ucn_v6_result_t ucn_v6_transfer_copy_tx(
    const ucn_v6_transfer_owner_t *owner,
    uint64_t message_id,
    ucn_v6_transfer_tx_view_t *view);
ucn_v6_result_t ucn_v6_transfer_retire_tx(
    ucn_v6_transfer_owner_t *owner,
    uint64_t message_id,
    uint64_t *buffer_token);
ucn_v6_result_t ucn_v6_transfer_rebind_path(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    uint64_t message_id,
    const ucn_v6_route_path_ref_t *route_ref);

ucn_v6_result_t ucn_v6_transfer_receive_fragment(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_security_open_result_t *opened,
    ucn_v6_transfer_rx_result_t *result);
ucn_v6_result_t ucn_v6_transfer_copy_completed(
    const ucn_v6_transfer_owner_t *owner,
    const ucn_v6_session_key_t *origin,
    uint64_t operation_id,
    uint64_t message_id,
    uint8_t *output,
    size_t output_capacity,
    ucn_v6_transfer_completed_t *completed);
ucn_v6_result_t ucn_v6_transfer_retire_completed(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_session_key_t *origin,
    uint64_t operation_id,
    uint64_t message_id);

ucn_v6_result_t ucn_v6_transfer_ingest_credit(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_security_open_result_t *opened,
    uint16_t policy_maximum_credit);
ucn_v6_result_t ucn_v6_transfer_reserve_credit(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_session_key_t *peer,
    uint16_t link_id,
    uint32_t link_generation,
    ucn_v6_traffic_class_t traffic_class,
    ucn_v6_transfer_credit_reservation_t *reservation);
ucn_v6_result_t ucn_v6_transfer_finish_credit(
    ucn_v6_transfer_owner_t *owner,
    uint64_t reservation_id,
    bool submitted);
ucn_v6_result_t ucn_v6_transfer_expire(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us);
/* EN: Atomically applies one canonical parent invalidation.  Every matching
 * local TX buffer token is returned, or the operation writes nothing.
 * 中文：原子应用一个规范父代际失效事件；调用方要么收到全部匹配的本地
 * TX Buffer Token，要么本操作完全不写入。 */
ucn_v6_result_t ucn_v6_transfer_apply_invalidation(
    ucn_v6_transfer_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation,
    uint64_t *retired_tx_buffer_tokens,
    size_t retired_capacity,
    size_t *retired_count,
    ucn_v6_transfer_invalidation_result_t *result);
ucn_v6_result_t ucn_v6_transfer_copy_stats(
    const ucn_v6_transfer_owner_t *owner,
    ucn_v6_transfer_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
