#ifndef UCN_V6_IDENTITY_H
#define UCN_V6_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_PROTOCOL_VERSION ((uint8_t)6U)
#define UCN_V6_SERIAL_ROTATION_THRESHOLD UINT32_C(0xFFFFFFFE)
#define UCN_V6_SERIAL64_ROTATION_THRESHOLD UINT64_C(0xFFFFFFFFFFFFFFFE)
#ifndef UCN_V6_CONFIG_MAX_BINDINGS
#define UCN_V6_CONFIG_MAX_BINDINGS 16U
#endif
#ifndef UCN_V6_CONFIG_MAX_ACTIVE_GROUPS
#define UCN_V6_CONFIG_MAX_ACTIVE_GROUPS 8U
#endif
#define UCN_V6_MAX_BINDING_SLOTS ((size_t)UCN_V6_CONFIG_MAX_BINDINGS)
#define UCN_V6_MAX_ACTIVE_GROUPS ((size_t)UCN_V6_CONFIG_MAX_ACTIVE_GROUPS)

typedef enum ucn_v6_result {
    UCN_V6_OK = 0,
    UCN_V6_ERR_ARGUMENT = -1,
    UCN_V6_ERR_CONFIG = -2,
    UCN_V6_ERR_NO_SPACE = -3,
    UCN_V6_ERR_MALFORMED = -4,
    UCN_V6_ERR_SECURITY = -5,
    UCN_V6_ERR_REPLAY = -6,
    UCN_V6_ERR_ACCESS = -7,
    UCN_V6_ERR_STATE = -8,
    UCN_V6_ERR_EXHAUSTED = -9,
    UCN_V6_ERR_NOT_FOUND = -10,
    UCN_V6_ERR_TIMEOUT = -11,
    UCN_V6_ERR_CANCELLED = -12
} ucn_v6_result_t;

typedef struct ucn_v6_principal {
    uint8_t bytes[16];
} ucn_v6_principal_t;

typedef struct ucn_v6_binding_key {
    uint32_t realm_id;
    uint32_t node_address;
    uint32_t binding_generation;
} ucn_v6_binding_key_t;

typedef struct ucn_v6_session_key {
    ucn_v6_binding_key_t binding;
    ucn_v6_principal_t principal;
    uint32_t session_generation;
} ucn_v6_session_key_t;

typedef enum ucn_v6_address_mode {
    UCN_V6_ADDRESS_STATIC = 1,
    UCN_V6_ADDRESS_LEASED = 2,
    UCN_V6_ADDRESS_SELF_PROPOSED = 3
} ucn_v6_address_mode_t;

typedef struct ucn_v6_authority_epoch {
    ucn_v6_principal_t authority_principal;
    uint32_t authority_generation;
    uint8_t durable_fence_token[16];
    uint64_t lease_sequence;
    uint64_t lease_duration_us;
    uint8_t allocation_high_water_digest[16];
    bool quorum_proven;
    bool durable;
} ucn_v6_authority_epoch_t;

typedef struct ucn_v6_lease_verifier_policy {
    uint32_t local_timer_max_slow_ppm;
    uint64_t local_timer_resolution_us;
    uint64_t local_timer_read_uncertainty_us;
    bool timer_read_uncertainty_known;
    uint64_t local_policy_max_lease_us;
} ucn_v6_lease_verifier_policy_t;

typedef struct ucn_v6_binding_certificate {
    ucn_v6_principal_t device_principal;
    ucn_v6_principal_t authority_principal;
    ucn_v6_binding_key_t binding;
    uint32_t authority_generation;
    uint8_t lease_id[16];
    uint64_t lease_duration_us;
    uint64_t authority_lease_sequence;
    ucn_v6_address_mode_t mode;
} ucn_v6_binding_certificate_t;

typedef struct ucn_v6_binding_slot {
    bool occupied;
    bool active;
    uint32_t node_address;
    uint32_t generation_high_water;
    ucn_v6_binding_certificate_t certificate;
} ucn_v6_binding_slot_t;

typedef struct ucn_v6_group_allocator {
    uint32_t dynamic_group_id_high_water;
    uint32_t active_group_ids[UCN_V6_MAX_ACTIVE_GROUPS];
} ucn_v6_group_allocator_t;

typedef struct ucn_v6_callback_gate {
    void *context;
    void (*lock)(void *context);
    void (*unlock)(void *context);
    const void *active_owner;
    bool initialized;
    bool active;
} ucn_v6_callback_gate_t;

typedef struct ucn_v6_identity_store_ops {
    void *context;
    ucn_v6_result_t (*persist_authority_epoch)(
        void *context,
        const ucn_v6_authority_epoch_t *epoch);
    ucn_v6_result_t (*persist_binding_slot)(
        void *context,
        const ucn_v6_binding_slot_t *slot);
    ucn_v6_result_t (*persist_group_high_water)(
        void *context,
        uint32_t group_id_high_water);
} ucn_v6_identity_store_ops_t;

typedef struct ucn_v6_identity_authority ucn_v6_identity_authority_t;
#ifndef UCN_V6_IDENTITY_AUTHORITY_STORAGE_BYTES
#define UCN_V6_IDENTITY_AUTHORITY_STORAGE_BYTES                       \
    ((size_t)(512U + UCN_V6_CONFIG_MAX_BINDINGS * 128U +             \
              UCN_V6_CONFIG_MAX_ACTIVE_GROUPS * 8U))
#endif
typedef union ucn_v6_identity_authority_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_IDENTITY_AUTHORITY_STORAGE_BYTES];
} ucn_v6_identity_authority_storage_t;

typedef struct ucn_v6_identity_authority_view {
    uint32_t realm_id;
    ucn_v6_authority_epoch_t epoch;
    uint64_t local_lease_deadline_us;
    uint32_t dynamic_group_id_high_water;
    uint16_t occupied_bindings;
    bool epoch_valid;
    bool faulted;
} ucn_v6_identity_authority_view_t;

struct ucn_v6_feature_manifest;

/* EN: Validates canonical identity and binding keys.
 * 中文：校验规范的身份与地址绑定键。 */
bool ucn_v6_principal_is_valid(const ucn_v6_principal_t *principal);
bool ucn_v6_binding_key_is_valid(const ucn_v6_binding_key_t *binding);
bool ucn_v6_binding_key_equal(
    const ucn_v6_binding_key_t *left,
    const ucn_v6_binding_key_t *right);
/* EN: Advances a no-wrap ownership serial or reports exhaustion.
 * 中文：推进不可回绕的所有权序列，耗尽时明确报错。 */
ucn_v6_result_t ucn_v6_serial_checked_next(
    uint32_t current,
    uint32_t *next);
/* EN: Builds and checks one conservative local half-open lease deadline.
 * 中文：建立并检查保守的本地半开租约截止期。 */
ucn_v6_result_t ucn_v6_lease_deadline_build(
    uint64_t challenge_started_local_us,
    uint64_t max_remaining_lease_us,
    const ucn_v6_lease_verifier_policy_t *policy,
    uint64_t *local_deadline_us);
bool ucn_v6_lease_deadline_is_live(uint64_t now_us, uint64_t deadline_us);

/* EN: Initializes a caller-owned, shared Provider callback fence.
 * 中文：初始化由调用方持有、跨对象共享的 Provider 回调围栏。 */
ucn_v6_result_t ucn_v6_callback_gate_init(
    ucn_v6_callback_gate_t *gate,
    void *context,
    void (*lock)(void *context),
    void (*unlock)(void *context));
/* EN: Reads the shared callback fence under its caller-supplied lock.
 * 中文：在调用方提供的锁保护下读取共享回调围栏。 */
bool ucn_v6_callback_gate_is_active(ucn_v6_callback_gate_t *gate);
/* EN: Enters or leaves the shared Provider callback dynamic extent.
 * 中文：进入或退出共享 Provider 回调动态范围。 */
ucn_v6_result_t ucn_v6_callback_gate_try_enter(
    ucn_v6_callback_gate_t *gate,
    const void *owner);
ucn_v6_result_t ucn_v6_callback_gate_leave(
    ucn_v6_callback_gate_t *gate,
    const void *owner);

/* EN: Initializes the isolated Realm Address Authority model.
 * 中文：初始化隔离的 Realm 地址权威模型。 */
ucn_v6_result_t ucn_v6_identity_authority_init_in_place(
    void *storage,
    size_t storage_bytes,
    const struct ucn_v6_feature_manifest *manifest,
    uint32_t realm_id,
    const ucn_v6_identity_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_identity_authority_t **authority);
/* EN: Persists and publishes an exact next Authority Epoch.
 * 中文：持久化并发布精确后继的地址权威 Epoch。 */
ucn_v6_result_t ucn_v6_identity_authority_install_epoch(
    ucn_v6_identity_authority_t *authority,
    const ucn_v6_authority_epoch_t *epoch,
    uint64_t local_lease_deadline_us);
/* EN: Allocates or retires Binding ownership with persist-before-publish.
 * 中文：按先持久化后发布原则分配或退休 Binding 所有权。 */
ucn_v6_result_t ucn_v6_identity_authority_allocate_binding(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t node_address,
    const ucn_v6_principal_t *device_principal,
    ucn_v6_address_mode_t mode,
    const uint8_t lease_id[16],
    uint64_t lease_duration_us,
    ucn_v6_binding_certificate_t *certificate);
ucn_v6_result_t ucn_v6_identity_authority_retire_binding(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t node_address,
    uint32_t binding_generation);
/* EN: Allocates or retires dynamic Group IDs without reusing holes.
 * 中文：不复用历史空洞地分配或退休动态 Group ID。 */
ucn_v6_result_t ucn_v6_identity_authority_allocate_dynamic_group(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t *group_id);
ucn_v6_result_t ucn_v6_identity_authority_retire_dynamic_group(
    ucn_v6_identity_authority_t *authority,
    uint32_t group_id);
/* EN: Copies a read-only diagnostic view after validating opaque storage.
 * 中文：校验 opaque storage 后复制只读诊断视图。 */
ucn_v6_result_t ucn_v6_identity_authority_copy_view(
    const ucn_v6_identity_authority_t *authority,
    ucn_v6_identity_authority_view_t *view);

#ifdef __cplusplus
}
#endif

#endif
