#ifndef UCN_V6_ADAPTER_H
#define UCN_V6_ADAPTER_H

/* Fixed-memory v6 driver boundary.
 *
 * An ISR/SDK callback may only publish a complete RX record or one TX
 * completion.  It never parses Wire, mutates routing, or runs application
 * code.  One serialized Protocol Owner peeks and retires records. */

#include "ucn/v6/ucn_v6_owner.h"
#include "ucn/v6/ucn_v6_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_ADAPTER_API_VERSION UINT16_C(1)

typedef enum ucn_v6_bearer_kind {
    UCN_V6_BEARER_UART = 1,
    UCN_V6_BEARER_RS485 = 2,
    UCN_V6_BEARER_ESP_NOW = 3,
    UCN_V6_BEARER_CAN = 4,
    UCN_V6_BEARER_CAN_FD = 5,
    UCN_V6_BEARER_USB = 6,
    UCN_V6_BEARER_CUSTOM = 255
} ucn_v6_bearer_kind_t;

typedef enum ucn_v6_driver_tx_state {
    UCN_V6_DRIVER_TX_FREE = 0,
    UCN_V6_DRIVER_TX_QUEUED = 1,
    UCN_V6_DRIVER_TX_SUBMITTING = 2,
    UCN_V6_DRIVER_TX_SUBMITTED = 3,
    UCN_V6_DRIVER_TX_COMPLETED = 4,
    UCN_V6_DRIVER_TX_CANCELLED = 5
} ucn_v6_driver_tx_state_t;

typedef enum ucn_v6_driver_link_readiness {
    UCN_V6_LINK_STARTING = 1,
    UCN_V6_LINK_READY = 2,
    UCN_V6_LINK_OFFLINE = 3,
    UCN_V6_LINK_QUIESCING = 4,
    UCN_V6_LINK_FAULTED = 5
} ucn_v6_driver_link_readiness_t;

typedef struct ucn_v6_driver_event_key {
    uint16_t link_id;
    uint32_t link_generation;
    uint32_t event_token;
} ucn_v6_driver_event_key_t;

typedef struct ucn_v6_driver_timestamp {
    uint64_t timestamp_us;
    uint32_t uncertainty_us;
    bool valid;
    bool hardware;
} ucn_v6_driver_timestamp_t;

typedef struct ucn_v6_driver_runtime_ops {
    void *context;
    ucn_v6_result_t (*lock_task)(void *context);
    bool (*try_lock_from_isr)(void *context);
    void (*unlock_task)(void *context);
    void (*unlock_from_isr)(void *context);
    ucn_v6_result_t (*post_owner_event)(
        void *context, ucn_v6_owner_event_t event, bool from_isr);
} ucn_v6_driver_runtime_ops_t;

typedef struct ucn_v6_driver_link_ops {
    size_t struct_size;
    uint16_t api_version;
    void *context;
    ucn_v6_result_t (*submit)(
        void *context,
        const ucn_v6_driver_event_key_t *key,
        const uint8_t *frame,
        size_t frame_length,
        uint8_t hardware_priority,
        bool request_timestamp);
    ucn_v6_result_t (*cancel)(
        void *context, const ucn_v6_driver_event_key_t *key);
    ucn_v6_result_t (*quiesce)(void *context);
} ucn_v6_driver_link_ops_t;

typedef struct ucn_v6_driver_link_config {
    uint16_t link_id;
    uint32_t initial_generation;
    ucn_v6_bearer_kind_t bearer;
    uint32_t nominal_bitrate_bps;
    uint16_t carrier_mtu;
    uint16_t link_frame_mtu;
    uint8_t hardware_priority_count;
    uint8_t rx_slot_quota;
    uint8_t tx_slot_quota;
    bool rx_timestamp_hardware;
    bool tx_timestamp_hardware;
    bool half_duplex;
    ucn_v6_driver_link_ops_t ops;
} ucn_v6_driver_link_config_t;

typedef struct ucn_v6_driver_rx_view {
    ucn_v6_driver_event_key_t key;
    ucn_v6_driver_timestamp_t timestamp;
    ucn_v6_bearer_kind_t bearer;
    uint16_t frame_length;
} ucn_v6_driver_rx_view_t;

typedef struct ucn_v6_driver_tx_completion {
    ucn_v6_driver_event_key_t key;
    ucn_v6_driver_timestamp_t timestamp;
    uint64_t buffer_token;
    ucn_v6_result_t result;
} ucn_v6_driver_tx_completion_t;

typedef struct ucn_v6_adapter_stats {
    uint32_t rx_published;
    uint32_t rx_retired;
    uint32_t rx_dropped_full;
    uint32_t rx_stale;
    uint32_t tx_queued;
    uint32_t tx_submitted;
    uint32_t tx_completed;
    uint32_t tx_retired;
    uint32_t tx_submit_failed;
    uint32_t tx_stale_completion;
    uint32_t link_reopens;
    uint32_t notification_failures;
    bool faulted;
} ucn_v6_adapter_stats_t;

typedef struct ucn_v6_driver_link_base {
    uint16_t link_id;
    uint32_t initial_generation;
    uint8_t rx_slot_quota;
    uint8_t tx_slot_quota;
    ucn_v6_driver_link_ops_t ops;
} ucn_v6_driver_link_base_t;

typedef struct ucn_v6_adapter_owner ucn_v6_adapter_owner_t;
typedef union ucn_v6_adapter_owner_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_ADAPTER_OWNER_STORAGE_BYTES];
} ucn_v6_adapter_owner_storage_t;

/* EN: Initializes one multi-Link Adapter in caller-owned storage.
 * 中文：在调用方存储中初始化一个多 Link Adapter。 */
ucn_v6_result_t ucn_v6_adapter_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_driver_runtime_ops_t *runtime_ops,
    ucn_v6_adapter_owner_t **owner);

/* EN: Registers an independent Link instance. Duplicate IDs are rejected.
 * 中文：注册独立 Link 实例；重复 ID 被拒绝。 */
ucn_v6_result_t ucn_v6_adapter_register_link(
    ucn_v6_adapter_owner_t *owner,
    const ucn_v6_driver_link_config_t *config);

/* EN: Commits Driver readiness for the exact Link generation.  An OFFLINE or
 * FAULTED instance must be reopened before it can become READY again.
 * 中文：为精确 Link 代际提交 Driver 就绪状态；OFFLINE 或 FAULTED 实例必须
 * 先 reopen，才可再次进入 READY。 */
ucn_v6_result_t ucn_v6_adapter_set_link_readiness(
    ucn_v6_adapter_owner_t *owner,
    uint16_t link_id,
    uint32_t link_generation,
    ucn_v6_driver_link_readiness_t readiness);

/* EN: Atomically copies frame+event-key+timestamp from an ISR or callback.
 * The caller retains its source buffer immediately after return.
 * 中文：从 ISR/回调原子复制帧、事件键和时间戳；返回后源 Buffer 仍归调用方。 */
ucn_v6_result_t ucn_v6_adapter_publish_rx(
    ucn_v6_adapter_owner_t *owner,
    uint16_t link_id,
    uint32_t link_generation,
    const uint8_t *frame,
    size_t frame_length,
    const ucn_v6_driver_timestamp_t *timestamp,
    bool from_isr,
    ucn_v6_driver_event_key_t *published_key);

/* EN: Peeks the oldest complete RX item; retirement is a separate commit.
 * 中文：预取最早的完整 RX 项；退休是独立提交步骤。 */
ucn_v6_result_t ucn_v6_adapter_peek_rx(
    ucn_v6_adapter_owner_t *owner,
    uint8_t *frame,
    size_t frame_capacity,
    ucn_v6_driver_rx_view_t *view);
ucn_v6_result_t ucn_v6_adapter_retire_rx(
    ucn_v6_adapter_owner_t *owner,
    const ucn_v6_driver_event_key_t *key);

/* EN: Copies an immutable encoded frame into a fixed TX slot. buffer_token is
 * returned only after physical completion/cancellation is retired.
 * 中文：把不可变编码帧复制进固定 TX 槽；仅在物理完成/取消被退休后返还
 * buffer_token。 */
ucn_v6_result_t ucn_v6_adapter_enqueue_tx(
    ucn_v6_adapter_owner_t *owner,
    uint16_t link_id,
    uint64_t buffer_token,
    const uint8_t *frame,
    size_t frame_length,
    ucn_v6_traffic_class_t traffic_class,
    bool request_timestamp,
    ucn_v6_driver_event_key_t *key);

/* EN: Performs at most one non-blocking physical submit from Owner context.
 * 中文：在 Owner 上下文中最多执行一次非阻塞物理提交。 */
ucn_v6_result_t ucn_v6_adapter_service_tx(
    ucn_v6_adapter_owner_t *owner,
    bool *submitted);

/* EN: Cancels one queued or submitted TX reservation.  Physical cancellation
 * is committed before the slot becomes retireable.
 * 中文：取消一个排队中或已提交的 TX 预留；物理取消提交成功后，槽位才可退休。 */
ucn_v6_result_t ucn_v6_adapter_cancel_tx(
    ucn_v6_adapter_owner_t *owner,
    const ucn_v6_driver_event_key_t *key);

/* EN: Publishes exactly one physical TX completion from task/ISR context.
 * 中文：从任务/ISR 上下文精确发布一次物理 TX 完成。 */
ucn_v6_result_t ucn_v6_adapter_publish_tx_completion(
    ucn_v6_adapter_owner_t *owner,
    const ucn_v6_driver_event_key_t *key,
    ucn_v6_result_t result,
    const ucn_v6_driver_timestamp_t *timestamp,
    bool from_isr);
ucn_v6_result_t ucn_v6_adapter_peek_tx_completion(
    ucn_v6_adapter_owner_t *owner,
    ucn_v6_driver_tx_completion_t *completion);
ucn_v6_result_t ucn_v6_adapter_retire_tx_completion(
    ucn_v6_adapter_owner_t *owner,
    const ucn_v6_driver_event_key_t *key,
    uint64_t *buffer_token);

/* EN: Quiesces one Driver, returns all owned TX buffers, advances the no-wrap
 * Link generation and rejects every late callback from the old instance.
 * 中文：静止 Driver、返还全部 TX Buffer、推进禁止回绕的 Link generation，
 * 并拒绝旧实例的全部迟到回调。 */
ucn_v6_result_t ucn_v6_adapter_reopen_link(
    ucn_v6_adapter_owner_t *owner,
    uint16_t link_id,
    uint64_t *retired_buffer_tokens,
    size_t retired_capacity,
    size_t *retired_count,
    uint32_t *new_generation);

ucn_v6_result_t ucn_v6_adapter_copy_stats(
    ucn_v6_adapter_owner_t *owner,
    ucn_v6_adapter_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
