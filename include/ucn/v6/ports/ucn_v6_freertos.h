#ifndef UCN_V6_FREERTOS_H
#define UCN_V6_FREERTOS_H

/* FreeRTOS integration boundary without an SDK header dependency.
 * Products map the callbacks below to task notifications and task/ISR-safe
 * critical sections in one small BSP file. */

#include "ucn/v6/ucn_v6_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_FREERTOS_PORT_API_VERSION UINT16_C(1)

typedef struct ucn_v6_freertos_port_ops {
    size_t struct_size;
    uint16_t api_version;
    void *context;
    ucn_v6_result_t (*lock_task)(void *context);
    bool (*try_lock_from_isr)(void *context);
    void (*unlock_task)(void *context);
    void (*unlock_from_isr)(void *context);
    void (*notify_owner_task)(void *context, bool from_isr);
    ucn_v6_result_t (*wait_for_notification)(
        void *context, uint64_t max_wait_us, bool *notified);
    ucn_v6_result_t (*read_monotonic_time_us)(
        void *context, uint64_t *now_us);
} ucn_v6_freertos_port_ops_t;

typedef ucn_v6_result_t (*ucn_v6_freertos_event_handler_t)(
    void *context,
    ucn_v6_owner_event_t event,
    uint64_t now_us,
    ucn_v6_adapter_owner_t *adapter);

typedef struct ucn_v6_freertos_port ucn_v6_freertos_port_t;
typedef union ucn_v6_freertos_port_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_FREERTOS_PORT_STORAGE_BYTES];
} ucn_v6_freertos_port_storage_t;

/* EN: Initializes the single Owner-task bridge.  No task is created here.
 * 中文：初始化唯一 Owner 任务桥；本函数不创建任务。 */
ucn_v6_result_t ucn_v6_freertos_port_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_freertos_port_ops_t *ops,
    ucn_v6_freertos_event_handler_t handler,
    void *handler_context,
    ucn_v6_freertos_port_t **port);

/* EN: Produces the exact Adapter runtime callbacks bound to this port.
 * 中文：生成精确绑定到本 Port 的 Adapter 运行时回调。 */
ucn_v6_result_t ucn_v6_freertos_port_make_adapter_runtime(
    ucn_v6_freertos_port_t *port,
    ucn_v6_driver_runtime_ops_t *runtime_ops);

/* EN: Binds the Adapter after both caller-owned objects are initialized.
 * 中文：在两个调用方对象初始化后绑定 Adapter。 */
ucn_v6_result_t ucn_v6_freertos_port_bind_adapter(
    ucn_v6_freertos_port_t *port,
    ucn_v6_adapter_owner_t *adapter);

/* EN: Runs at most budget events in Owner-task context.
 * 中文：在 Owner 任务上下文中最多处理 budget 个事件。 */
ucn_v6_result_t ucn_v6_freertos_port_run(
    ucn_v6_freertos_port_t *port,
    uint16_t budget,
    uint16_t *processed);

/* EN: Blocks only when no event is pending. Timeout becomes one TIMER event,
 * preserving polling as a bounded fallback rather than the primary path.
 * 中文：仅在无待处理事件时阻塞；超时转换为一个 TIMER 事件，使轮询只作为
 * 有界保底路径而非主路径。 */
ucn_v6_result_t ucn_v6_freertos_port_wait_and_run(
    ucn_v6_freertos_port_t *port,
    uint64_t max_wait_us,
    uint16_t budget,
    uint16_t *processed);

ucn_v6_result_t ucn_v6_freertos_port_post_timer(
    ucn_v6_freertos_port_t *port,
    bool from_isr);

#ifdef __cplusplus
}
#endif

#endif
