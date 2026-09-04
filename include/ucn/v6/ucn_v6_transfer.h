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
    ucn_v6_route_domain_t route_domain;
    ucn_v6_path_capability_t path;
    ucn_v6_message_descriptor_t message;
    ucn_v6_message_class_t message_class;
    uint64_t message_id;
    uint64_t buffer_token;
    const uint8_t *payload;
    uint16_t payload_length;
    uint16_t fragment_data_budget;
    uint16_t window_size;
} ucn_v6_transfer_send_request_t;

/* EN: message_id is the durable operation_id. The caller owns payload and
 * must keep it immutable until retire_tx returns its buffer_token.
 * 中文：message_id 即持久 Operation ID；payload 由调用方持有，并须保持
 * 不变，直到 retire_tx 返还 buffer_token。 */

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

ucn_v6_result_t ucn_v6_transfer_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
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
    const ucn_v6_path_capability_t *path);

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
/* EN: Atomically fences every resource owned by one revoked Peer Session.
 * Caller receives all local TX buffer tokens or the operation writes nothing.
 * 中文：原子撤销一个已失效 Peer Session 拥有的全部资源；调用方须有足够
 * 空间接收全部本地 TX Buffer Token，否则本操作不写入。 */
ucn_v6_result_t ucn_v6_transfer_invalidate_session(
    ucn_v6_transfer_owner_t *owner,
    const ucn_v6_session_key_t *session,
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
