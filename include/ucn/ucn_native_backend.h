#ifndef UCN_NATIVE_BACKEND_H
#define UCN_NATIVE_BACKEND_H
#include "ucn/ucn_native.h"

/* EN: Trusted product/Coordinator SPI, never application authentication evidence.
 * ZH: 可信产品装配/Coordinator SPI；业务不得借此自行声称认证。
 * The backend owns Wire/Security/Replay/Driver lifetimes, not the facade. */
typedef struct ucn_native_scope {
    uint32_t instance, generation;
    uint16_t slot, reserved_zero;
} ucn_native_scope_t;
/* Optional permanent Coordinator-owned hot-lifecycle memory, zero initialized.
 * Ordinary Native storage/capacities are unchanged. Never reuse while attached. */
#define UCN_NATIVE_LIFECYCLE_STORAGE_BYTES \
    (8U + UCN_NATIVE_CAPACITY * (13U * UCN_NATIVE_SOURCES + 12U * UCN_NATIVE_TARGETS + 4U))
typedef union ucn_native_lifecycle_storage {
    uint64_t alignment;
    unsigned char bytes[UCN_NATIVE_LIFECYCLE_STORAGE_BYTES];
} ucn_native_lifecycle_storage_t;
typedef struct ucn_native_borrowed_range {
    const void *address;
    size_t bytes;
} ucn_native_borrowed_range_t;
#ifdef __cplusplus
extern "C" {
#endif
ucn_result_t ucn_native_backend_enable_lifecycle(ucn_native_t *n,
    ucn_native_lifecycle_storage_t *storage, size_t bytes);
/* Trusted setup only: query after enable_lifecycle and before start. The
 * permanent borrowed range is immutable through deinit; it is not writable
 * application storage. Output must not alias any Native-owned/borrowed range. */
ucn_result_t ucn_native_backend_lifecycle_range(ucn_native_t *n,
    ucn_native_borrowed_range_t *range_out);
/* Cancellation barrier, never an authentication grant or physical TX drain.
 * Call before replacing the backend Session, outside step/dispatch/callback.
 * Same/older retirement is idempotent; namespaces cannot be rebound. */
ucn_result_t ucn_native_backend_retire_peer(ucn_native_t *n,
    ucn_native_source_t source, ucn_native_scope_t old_scope);
ucn_result_t ucn_native_backend_peer_quiescent(ucn_native_t *n,
    ucn_native_source_t source, ucn_native_scope_t old_scope, bool *out);
/* Exact Facade/backend ownership check before cross-layer lifecycle changes.
 * 跨层生命周期变更前核对Facade/Backend精确绑定；不授予消息权限。 */
ucn_result_t ucn_native_backend_check_binding(ucn_native_t *n,
    const ucn_native_backend_t *backend);
#ifdef __cplusplus
}
#endif
typedef struct ucn_native_backend_message {
    ucn_native_source_t source;
    ucn_native_scope_t scope;
    uint64_t receive_time_us;
    uint32_t origin_sequence;
    uint16_t service_id, payload_bytes;
    uint8_t traffic_class, protection;
} ucn_native_backend_message_t;
struct ucn_native_backend {
    uint16_t struct_size, api_version;
    void *context;
    size_t context_bytes;
    /* Pure, zero-write validation of supported Service/protection/shape. */
    ucn_result_t (*preflight)(void *, const ucn_native_channel_t *);
    ucn_result_t (*local_view)(void *, ucn_node_view_t *);
    ucn_result_t (*binding)(void *, uint32_t, ucn_native_source_t *);
    ucn_result_t (*start)(void *, uint64_t);
    ucn_result_t (*stop)(void *);
    ucn_result_t (*step)(void *, uint64_t, uint16_t);
    ucn_result_t (*send)(void *, const ucn_native_channel_t *, ucn_native_target_t,
                         const uint8_t *, uint64_t, uint64_t);
    /* Return authenticated, ACL-checked copied bytes and the ORIGINAL RX time.
     * Replay is committed once by backend. Empty queue returns NOT_FOUND. */
    ucn_result_t (*poll)(void *, ucn_native_backend_message_t *, uint8_t *, size_t);
    /* Exact scope/current ACL check before EACH app callback; output untouched on error.
     * Scope zero is only local delivery, never a remote authentication grant. */
    ucn_result_t (*validate)(void *, const ucn_native_scope_t *,
        const ucn_native_source_t *, uint16_t, uint8_t, uint64_t, uint8_t[16]);
};
#endif
