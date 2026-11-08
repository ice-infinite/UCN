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
