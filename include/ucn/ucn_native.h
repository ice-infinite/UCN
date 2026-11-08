#ifndef UCN_NATIVE_H
#define UCN_NATIVE_H
#include "ucn/ucn_core.h"

/* EN: Optional, static application facade. No Wire fields or hidden heap.
 * 中文：可选静态业务接口，不增加Wire字段，不隐式分配堆。 */
#define UCN_NATIVE_API_VERSION UINT16_C(2)
#define UCN_NATIVE_LAYOUT UINT16_C(2)
#define UCN_NATIVE_PAYLOAD_BYTES 64U
#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_NATIVE_CAPACITY 4U
#define UCN_NATIVE_TARGETS 2U
#define UCN_NATIVE_SOURCES 4U
#define UCN_NATIVE_STORAGE_BYTES 2048U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_NATIVE_CAPACITY 8U
#define UCN_NATIVE_TARGETS 4U
#define UCN_NATIVE_SOURCES 8U
#define UCN_NATIVE_STORAGE_BYTES 6144U
#else
#define UCN_NATIVE_CAPACITY 16U
#define UCN_NATIVE_TARGETS 8U
#define UCN_NATIVE_SOURCES 16U
#define UCN_NATIVE_STORAGE_BYTES 12288U
#endif
/* All layout/capacity values participate; this is not a security MAC. */
#define UCN_NATIVE_I_H1 UCN_I_MANIFEST_FNV_U32(UCN_COMPILED_MANIFEST_HASH, UCN_NATIVE_API_VERSION)
#define UCN_NATIVE_I_H2 UCN_I_MANIFEST_FNV_U32(UCN_NATIVE_I_H1, UCN_NATIVE_LAYOUT)
#define UCN_NATIVE_I_H3 UCN_I_MANIFEST_FNV_U32(UCN_NATIVE_I_H2, UCN_NATIVE_CAPACITY)
#define UCN_NATIVE_I_H4 UCN_I_MANIFEST_FNV_U32(UCN_NATIVE_I_H3, UCN_NATIVE_TARGETS)
#define UCN_NATIVE_I_H5 UCN_I_MANIFEST_FNV_U32(UCN_NATIVE_I_H4, UCN_NATIVE_SOURCES)
#define UCN_NATIVE_I_H6 UCN_I_MANIFEST_FNV_U32(UCN_NATIVE_I_H5, UCN_NATIVE_PAYLOAD_BYTES)
#define UCN_NATIVE_I_H7 UCN_I_MANIFEST_FNV_U32(UCN_NATIVE_I_H6, UCN_NATIVE_STORAGE_BYTES)
#define UCN_NATIVE_I_H8 UCN_I_MANIFEST_FNV_U32(UCN_NATIVE_I_H7, sizeof(ucn_native_channel_t))
#define UCN_NATIVE_I_H9 UCN_I_MANIFEST_FNV_U32(UCN_NATIVE_I_H8, sizeof(ucn_native_sample_t))
#define UCN_NATIVE_I_H10 UCN_I_MANIFEST_FNV_U32(UCN_NATIVE_I_H9, sizeof(ucn_native_config_t))
#define UCN_NATIVE_MANIFEST_HASH ((uint64_t)UCN_I_MANIFEST_FNV_U32(UCN_NATIVE_I_H10, sizeof(ucn_native_handle_t)))

#ifdef __cplusplus
extern "C" {
#endif
typedef struct ucn_native ucn_native_t;
typedef struct ucn_native_backend ucn_native_backend_t;
typedef union ucn_native_storage {
    uint64_t alignment;
    unsigned char bytes[UCN_NATIVE_STORAGE_BYTES];
} ucn_native_storage_t;
typedef struct ucn_native_handle {
    uint32_t instance;
    uint32_t generation;
    uint16_t slot;
    uint8_t kind;
    uint8_t reserved_zero;
} ucn_native_handle_t;
typedef struct ucn_native_source {
    uint32_t address;
    uint32_t binding_generation;
    uint64_t principal_digest;
} ucn_native_source_t;
typedef struct ucn_native_target {
    uint32_t address;
    uint32_t binding_generation;
} ucn_native_target_t;
enum { UCN_NATIVE_FIFO = 1, UCN_NATIVE_KEEP_LATEST = 2 };
enum { UCN_NATIVE_STATIC_TRUSTED = 1, UCN_NATIVE_ORIGIN_AUTH = 2,
       UCN_NATIVE_ENCRYPTED = 3, UCN_NATIVE_LOCAL = 4 };
typedef struct ucn_native_channel {
    uint64_t max_local_age_us;
    uint32_t schema_id;
    uint16_t id, service_id, schema_revision, payload_bytes;
    uint8_t traffic_class, queue_policy, security_minimum, reserved_zero;
    uint16_t source_count, target_count;
    ucn_native_source_t sources[UCN_NATIVE_SOURCES];
    ucn_native_target_t targets[UCN_NATIVE_TARGETS];
} ucn_native_channel_t;
typedef struct ucn_native_config {
    uint16_t api_version, storage_layout;
    uint32_t instance;
    uint64_t manifest_hash;
    const ucn_native_channel_t *channels;
    uint16_t channel_count;
    uint16_t reserved_zero;
    ucn_lock_ops_t lock;
} ucn_native_config_t;
typedef struct ucn_native_sample {
    ucn_native_source_t source;
    uint64_t receive_time_us;
    uint32_t origin_sequence;
    uint16_t channel_id, payload_bytes;
    uint8_t provenance;
    /* Full authenticated Principal for protected samples; zero for O0/local.
     * 保护样本的完整认证身份；O0/本地样本不伪造认证身份。 */
    uint8_t principal[16];
    /* Borrowed only during callback. 借用仅在回调内有效。 */
    const uint8_t *payload;
} ucn_native_sample_t;
typedef void (*ucn_native_receive_fn)(void *, const ucn_native_sample_t *);
typedef struct ucn_native_status {
    uint32_t accepted, core_admitted, failed, delivered, dropped;
    uint16_t pending_commands, pending_events;
    uint8_t stopping, core_quiescent;
} ucn_native_status_t;

/* Init is exclusive, storage must be all zero, Core pristine/initialized.
 * 初始化必须独占，静态存储全零，Core尚未使用Endpoint。 */
ucn_result_t ucn_native_init(void *storage, size_t storage_bytes, ucn_node_t *core,
                            const ucn_native_config_t *config, ucn_native_t **out_native);
/* EN: Exclusive trusted product assembly; backend/storage/context outlive Root.
 * ZH: 独占可信产品装配；后端、存储、context须保持到真实排空之后。 */
ucn_result_t ucn_native_init_backend(void *storage, size_t storage_bytes,
    const ucn_native_backend_t *backend, const ucn_native_config_t *config,
    ucn_native_t **out_native);
/* EN: Start the one underlying Core, once. 中文：仅启动底层唯一Core一次。 */
ucn_result_t ucn_native_start(ucn_native_t *native, uint64_t now_us);
/* EN: Exact typed Codec preflight, no entity allocation. 中文：精确核验Codec，不消耗实体槽。 */
ucn_result_t ucn_native_check_schema(ucn_native_t *native, uint16_t channel_id, uint32_t schema_id, uint16_t revision, uint16_t payload_bytes);
/* EN: Open exact non-reusable entity generations. 中文：创建精确代际实体，不复用历史句柄。 */
ucn_result_t ucn_publisher_create(ucn_native_t *native, uint16_t channel_id, ucn_native_handle_t *out_publisher);
ucn_result_t ucn_subscription_create(ucn_native_t *native, uint16_t channel_id, ucn_native_receive_fn receive, void *context, ucn_native_handle_t *out_subscription);
/* EN: Copy into a bounded deferred job; OK is LOCAL admission only.
 * 中文：复制到有界延迟命令，成功只代表本地接纳，不代表远端收到。 */
ucn_result_t ucn_publish(ucn_native_t *native, ucn_native_handle_t publisher, const void *payload, size_t payload_bytes);
ucn_result_t ucn_native_send(ucn_native_t *native, uint16_t channel_id, ucn_native_target_t target, const void *payload, size_t payload_bytes);
/* EN: Fixed close latch never needs a command slot. 中文：关闭围栏不依赖数据命令槽。 */
ucn_result_t ucn_publisher_close(ucn_native_t *native, ucn_native_handle_t publisher);
ucn_result_t ucn_subscription_close(ucn_native_t *native, ucn_native_handle_t subscription);
/* After close, context may be released only when this reports true.
 * 关闭后须等此查询为true，才能释放回调context。 */
ucn_result_t ucn_subscription_quiescent(ucn_native_t *native, ucn_native_handle_t subscription, bool *out_quiescent);
/* EN: Fence receive events and not-yet-Core-admitted local jobs; no physical recall.
 * 中文：围栏接收事件及尚未交给Core的本机命令，不冒充物理撤回。 */
ucn_result_t ucn_native_source_fence(ucn_native_t *native, uint16_t channel_id, ucn_native_source_t source);
/* EN: Bounded, serialized protocol and app executors; no recursive calls.
 * 中文：有界串行协议/应用执行器，禁止递归调用。 */
ucn_result_t ucn_native_step(ucn_native_t *native, uint64_t now_us, uint16_t budget);
ucn_result_t ucn_native_dispatch(ucn_native_t *native, uint64_t now_us, uint16_t budget, uint16_t *out_delivered);
ucn_result_t ucn_native_stop(ucn_native_t *native);
ucn_result_t ucn_native_status(ucn_native_t *native, ucn_native_status_t *out_status);
/* Deinit requires real quiescence; it never clears a live Driver token.
 * Root storage and its lock remain valid until ALL concurrent callers finish.
 * 必须真实排空，不得伪造Driver退休；Root和短锁须保持到所有并发调用者退出。 */
ucn_result_t ucn_native_deinit(ucn_native_t *native);
#ifdef __cplusplus
}
#endif
#endif
