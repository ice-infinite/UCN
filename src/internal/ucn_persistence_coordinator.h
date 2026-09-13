#ifndef UCN_INTERNAL_PERSISTENCE_COORDINATOR_H
#define UCN_INTERNAL_PERSISTENCE_COORDINATOR_H

#include "internal/ucn_coordinator.h"
#include "internal/ucn_persistence.h"

#define UCN_I_PERSIST_COORDINATOR_ADAPTER_MAGIC UINT32_C(0x55435041)
#define UCN_I_PERSIST_COORDINATOR_ADAPTER_SCHEMA UINT16_C(1)

typedef struct ucn_i_persist_route_binding {
    ucn_handle_t handle;
    uint64_t requirement_digest;
    uint8_t valid;
    uint8_t reserved_zero[7];
} ucn_i_persist_route_binding_t;

/* EN: Coordinator-only staging is caller-owned and physically separate from
 * the Persistence Foundation Owner. Every field is protected by the
 * Foundation Owner lock; consumer_references fences Owner destruction.
 * 中文：Coordinator 专用暂存由调用方持有，并与 Persistence Foundation
 * Owner 物理解耦。全部字段受 Foundation Owner 锁保护，consumer_references
 * 用于阻止 Owner 在 Adapter 存活期间被销毁。 */
typedef struct ucn_i_persistence_coordinator_adapter {
    uint32_t magic;
    uint16_t schema;
    uint8_t route_active;
    uint8_t bound;
    ucn_persistence_owner_t *owner;
    ucn_i_coordinator_t *coordinator;
    const ucn_persistence_request_t *route_request;
    uint64_t route_now_us;
    uint8_t route_exact[UCN_I_DEPENDENCY_EXACT_BYTES];
    ucn_handle_t route_handle;
    ucn_i_dependency_requirement_t route_requirement;
    ucn_i_persist_route_binding_t bindings[UCN_PERSIST_DOMAIN_COUNT];
} ucn_i_persistence_coordinator_adapter_t;

ucn_result_t ucn_i_persistence_coordinator_adapter_init(
    ucn_i_persistence_coordinator_adapter_t *adapter,
    ucn_persistence_owner_t *owner);
ucn_result_t ucn_i_persistence_coordinator_adapter_deinit(
    ucn_i_persistence_coordinator_adapter_t *adapter,
    const ucn_i_coordinator_t *destroyed_coordinator);
ucn_result_t ucn_i_persistence_bind_coordinator(
    ucn_i_persistence_coordinator_adapter_t *adapter,
    ucn_i_coordinator_t *coordinator);
ucn_result_t ucn_i_persistence_route_request(
    ucn_i_coordinator_t *coordinator,
    ucn_i_persistence_coordinator_adapter_t *adapter,
    const ucn_persistence_request_t *request,
    uint64_t now_us,
    ucn_handle_t *handle_out);
ucn_result_t ucn_i_persistence_route_terminal(
    ucn_i_coordinator_t *coordinator,
    ucn_i_persistence_coordinator_adapter_t *adapter,
    ucn_handle_t handle,
    uint64_t now_us);

#endif
