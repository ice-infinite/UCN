#ifndef UCN_TIMED_LINK_H
#define UCN_TIMED_LINK_H

/* Optional timestamp-capable Link extension and atomic event queues.
 * 可选的时间戳 Link 扩展与原子事件队列。 */

#include "ucn/ucn_link.h"
#include "ucn/ucn_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_TIME_LINK_OPS_API_VERSION ((uint16_t)1U)
#define UCN_TIME_EVENT_SERIAL_MAX UINT32_C(0x7FFFFFFF)

#ifndef UCN_TIME_TX_EVENT_QUEUE_DEPTH
#define UCN_TIME_TX_EVENT_QUEUE_DEPTH ((size_t)4U)
#endif
#ifndef UCN_TIME_TIMED_RX_QUEUE_DEPTH
#define UCN_TIME_TIMED_RX_QUEUE_DEPTH ((size_t)2U)
#endif
#ifndef UCN_TIME_EVENT_RESERVATION_CAPACITY
#define UCN_TIME_EVENT_RESERVATION_CAPACITY \
    (UCN_TIME_TX_EVENT_QUEUE_DEPTH + UCN_TIME_TIMED_RX_QUEUE_DEPTH)
#endif

typedef char ucn_time_tx_event_depth_must_be_nonzero[
    UCN_TIME_TX_EVENT_QUEUE_DEPTH > 0U ? 1 : -1];
typedef char ucn_time_rx_depth_must_be_nonzero[
    UCN_TIME_TIMED_RX_QUEUE_DEPTH > 0U ? 1 : -1];
typedef char ucn_time_reservation_capacity_must_be_nonzero[
    UCN_TIME_EVENT_RESERVATION_CAPACITY > 0U ? 1 : -1];

typedef uint8_t ucn_time_event_direction_t;
enum {
    UCN_TIME_EVENT_TX = 1U,
    UCN_TIME_EVENT_RX = 2U
};

typedef uint8_t ucn_time_event_role_t;
enum {
    UCN_TIME_EVENT_T1_TX = 1U,
    UCN_TIME_EVENT_T2_RX = 2U,
    UCN_TIME_EVENT_T3_TX = 3U,
    UCN_TIME_EVENT_T4_RX = 4U
};

typedef struct ucn_time_event_key {
    uint8_t link_id;
    ucn_time_event_direction_t direction;
    uint32_t link_instance_generation;
    uint32_t event_token;
} ucn_time_event_key_t;

typedef uint8_t ucn_time_event_lifecycle_t;
enum {
    UCN_TIME_EVENT_LIFECYCLE_FREE = 0U,
    UCN_TIME_EVENT_LIFECYCLE_RESERVED = 1U,
    UCN_TIME_EVENT_LIFECYCLE_SUBMITTED = 2U
};

typedef struct ucn_time_event_reservation {
    ucn_time_event_key_t key;
    ucn_time_event_lifecycle_t lifecycle;
    bool occupied;
} ucn_time_event_reservation_t;

/* EN: One execution domain owns one shared callback gate. Every Timed Link
 * that can run concurrently, including task and ISR paths, must reference the
 * same gate and its task/ISR callbacks must protect the same underlying lock.
 * 中文：一个执行域拥有一个共享回调围栏。所有可能并发运行的 Timed Link（含
 * 任务与 ISR 路径）必须引用同一围栏，且任务/ISR 回调必须保护同一底层锁。 */
typedef struct ucn_timed_link_callback_gate {
    const ucn_port_ops_t *port_ops;
    void *port_context;
    const void *active_owner;
    bool initialized;
    bool active;
} ucn_timed_link_callback_gate_t;

typedef struct ucn_time_link_ops {
    size_t struct_size;
    uint16_t api_version;
    ucn_result_t (*reserve_tx_token)(void *context,
                                     const ucn_time_event_key_t *key);
    ucn_result_t (*submit_timestamped)(void *context,
                                       const ucn_time_event_key_t *key,
                                       const uint8_t *frame,
                                       size_t length);
    ucn_result_t (*cancel_tx_token)(void *context,
                                    const ucn_time_event_key_t *key);
    /* Must stop the hardware callback source and drain its private queue
     * before a new Link Instance generation is exposed. */
    ucn_result_t (*quiesce)(void *context);
} ucn_time_link_ops_t;

typedef struct ucn_timed_link_stats {
    uint32_t keys_reserved;
    uint32_t timestamped_submissions;
    uint32_t cancelled_tokens;
    uint32_t reopen_count;
    uint32_t completed_tokens;
    uint32_t retired_tokens;
    uint32_t queue_full_drops;
    uint32_t stale_events;
} ucn_timed_link_stats_t;

typedef struct ucn_timed_link {
    const ucn_time_link_ops_t *ops;
    void *context;
    const ucn_port_ops_t *port_ops;
    void *port_context;
    ucn_timed_link_callback_gate_t *callback_gate;
    ucn_timed_link_stats_t stats;
    uint32_t link_instance_generation;
    uint32_t next_tx_token;
    uint32_t next_rx_token;
    ucn_time_event_reservation_t
        reservations[UCN_TIME_EVENT_RESERVATION_CAPACITY];
    uint8_t link_id;
    bool initialized;
    bool faulted;
    bool io_active;
} ucn_timed_link_t;

/* EN: Timed-Link control APIs are owned by one serialized protocol task.
 * Driver callbacks may publish timestamp events through the bounded queues,
 * but they must not call Timed-Link control APIs recursively.  Task/ISR queue
 * safety requires the matching ucn_port_ops_t lock pairs.
 * 中文：Timed-Link 控制 API 由单一串行协议任务拥有。驱动回调可通过有界
 * 队列发布时间戳事件，但不得递归调用 Timed-Link 控制 API；任务/ISR 队列
 * 安全依赖配套的 ucn_port_ops_t 临界区回调对。 */

typedef struct ucn_time_tx_timestamp_event {
    ucn_time_event_key_t key;
    uint64_t timestamp_us;
    uint8_t quality;
    ucn_result_t completion;
} ucn_time_tx_timestamp_event_t;

typedef struct ucn_time_timed_rx_item {
    ucn_link_t *ingress_link;
    ucn_time_event_key_t key;
    uint64_t timestamp_us;
    uint8_t quality;
    uint16_t length;
    uint8_t data[UCN_MAX_FRAME_BYTES];
} ucn_time_timed_rx_item_t;

typedef struct ucn_time_tx_event_queue {
    ucn_time_tx_timestamp_event_t items[UCN_TIME_TX_EVENT_QUEUE_DEPTH];
    size_t head;
    size_t tail;
    size_t count;
    const ucn_port_ops_t *port_ops;
    void *port_context;
    uint32_t enqueued;
    uint32_t dropped_full;
    uint32_t dequeued;
    bool initialized;
} ucn_time_tx_event_queue_t;

typedef struct ucn_time_timed_rx_queue {
    ucn_time_timed_rx_item_t items[UCN_TIME_TIMED_RX_QUEUE_DEPTH];
    size_t head;
    size_t tail;
    size_t count;
    const ucn_port_ops_t *port_ops;
    void *port_context;
    uint32_t enqueued;
    uint32_t dropped_full;
    uint32_t dequeued;
    bool initialized;
} ucn_time_timed_rx_queue_t;

/* EN: Checks a Time Link extension ABI before reading callbacks.
 * 中文：读取回调前检查 Time Link 扩展 ABI。 */
bool ucn_time_link_ops_is_compatible(const ucn_time_link_ops_t *ops);

/* EN: Initializes the caller-owned task/ISR/SMP-safe gate shared by every
 * Timed Link in one execution domain.  This is a pre-concurrency operation:
 * once any Link references the gate, it must not be reinitialized or moved.
 * If the gate and a per-Link lock use the same physical primitive, that
 * primitive must support the gate-before-Link nesting used by this module.
 * 中文：初始化由调用方持有、供同一执行域全部 Timed Link 共享且任务/ISR/SMP
 * 安全的围栏。这是并发开始前的操作；一旦被任一 Link 引用，围栏不得重新初始化
 * 或移动。若围栏锁与单 Link 锁使用同一物理原语，该原语必须支持本模块采用的
 * “围栏锁先于 Link 锁”嵌套顺序。 */
ucn_result_t ucn_timed_link_callback_gate_init(
    ucn_timed_link_callback_gate_t *gate,
    const ucn_port_ops_t *port_ops,
    void *port_context);

/* EN: Initializes one timestamp-capable Link instance.
 * 中文：初始化一个具备时间戳能力的 Link 实例。 */
ucn_result_t ucn_timed_link_init(ucn_timed_link_t *link,
                                 uint8_t link_id,
                                 uint32_t initial_generation,
                                 const ucn_time_link_ops_t *ops,
                                 void *context,
                                 const ucn_port_ops_t *port_ops,
                                 void *port_context,
                                 ucn_timed_link_callback_gate_t *callback_gate);

/* EN: Allocates one no-wrap local event key from task context.
 * 中文：从任务上下文分配一个禁止回绕的本地事件键。 */
ucn_result_t ucn_timed_link_allocate_event(
    ucn_timed_link_t *link,
    ucn_time_event_direction_t direction,
    ucn_time_event_key_t *key);

/* EN: Allocates one RX event key from ISR context using the ISR lock pair.
 * 中文：使用 ISR 锁对从中断上下文分配一个 RX 事件键。 */
ucn_result_t ucn_timed_link_allocate_rx_event_from_isr(
    ucn_timed_link_t *link,
    ucn_time_event_key_t *key);

/* EN: Atomically submits a complete frame with its reserved TX key.
 * 中文：把完整帧与已保留 TX key 原子提交给驱动。 */
ucn_result_t ucn_timed_link_submit(ucn_timed_link_t *link,
                                   const ucn_time_event_key_t *key,
                                   const uint8_t *frame,
                                   size_t length);

/* EN: Cancels one locally reserved TX event.
 * 中文：取消一个本地已保留的 TX 事件。 */
ucn_result_t ucn_timed_link_cancel(ucn_timed_link_t *link,
                                   const ucn_time_event_key_t *key);

/* EN: Completes one submitted TX or captured RX reservation exactly once.
 * 中文：精确一次完成已提交 TX 或已捕获 RX 的 reservation。 */
ucn_result_t ucn_timed_link_complete_event(
    ucn_timed_link_t *link,
    const ucn_time_event_key_t *key);

/* EN: Retires an abandoned reservation. TX retirement asks the driver to
 * release hardware state; RX retirement is local. Submitted TX retirement is
 * only for the serialized Owner's timeout/reopen cleanup, not normal cancel.
 * 中文：回收废弃 reservation；TX 会请求驱动释放硬件状态，RX 仅本地回收。
 * 已提交 TX 的 retire 只供串行 Owner 的超时/切路清理，不等同普通 cancel。 */
ucn_result_t ucn_timed_link_retire_event(
    ucn_timed_link_t *link,
    const ucn_time_event_key_t *key);

/* EN: Quiesces the driver and opens the next no-wrap Link Instance.
 * 中文：静止驱动后打开下一个禁止回绕的 Link Instance。 */
ucn_result_t ucn_timed_link_reopen(ucn_timed_link_t *link);

/* EN: Returns whether a callback key belongs to the current Link Instance.
 * 中文：判断回调 key 是否属于当前 Link Instance。 */
bool ucn_timed_link_key_is_current(const ucn_timed_link_t *link,
                                   const ucn_time_event_key_t *key);

/* EN: Initializes the bounded TX timestamp completion queue.
 * 中文：初始化有界 TX 时间戳完成队列。 */
ucn_result_t ucn_time_tx_event_queue_init(
    ucn_time_tx_event_queue_t *queue,
    const ucn_port_ops_t *port_ops,
    void *port_context);

/* EN: Enqueues one TX completion from task context.
 * 中文：从任务上下文入队一个 TX 完成事件。 */
ucn_result_t ucn_time_tx_event_enqueue(
    ucn_time_tx_event_queue_t *queue,
    const ucn_time_tx_timestamp_event_t *event);

/* EN: Enqueues one TX completion using the ISR lock pair.
 * 中文：使用 ISR 锁对入队一个 TX 完成事件。 */
ucn_result_t ucn_time_tx_event_enqueue_from_isr(
    ucn_time_tx_event_queue_t *queue,
    const ucn_time_tx_timestamp_event_t *event);

/* EN: Dequeues one complete TX timestamp event for the Owner.
 * 中文：为 Owner 出队一个完整 TX 时间戳事件。 */
ucn_result_t ucn_time_tx_event_dequeue(
    ucn_time_tx_event_queue_t *queue,
    ucn_time_tx_timestamp_event_t *event);

/* EN: Initializes the bounded atomic Timed RX queue.
 * 中文：初始化有界原子 Timed RX 队列。 */
ucn_result_t ucn_time_timed_rx_queue_init(
    ucn_time_timed_rx_queue_t *queue,
    const ucn_port_ops_t *port_ops,
    void *port_context);

/* EN: Enqueues an inseparable frame/key/timestamp item from task context.
 * 中文：从任务上下文入队不可拆分的帧/key/时间戳记录。 */
ucn_result_t ucn_time_timed_rx_enqueue(
    ucn_time_timed_rx_queue_t *queue,
    const ucn_time_timed_rx_item_t *item);

/* EN: Enqueues an atomic Timed RX item using the ISR lock pair.
 * 中文：使用 ISR 锁对入队原子 Timed RX 记录。 */
ucn_result_t ucn_time_timed_rx_enqueue_from_isr(
    ucn_time_timed_rx_queue_t *queue,
    const ucn_time_timed_rx_item_t *item);

/* EN: Dequeues one complete Timed RX item for protocol processing.
 * 中文：为协议处理出队一个完整 Timed RX 记录。 */
ucn_result_t ucn_time_timed_rx_dequeue(
    ucn_time_timed_rx_queue_t *queue,
    ucn_time_timed_rx_item_t *item);

#ifdef __cplusplus
}
#endif

#endif
