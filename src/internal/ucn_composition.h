#ifndef UCN_COMPOSITION_H
#define UCN_COMPOSITION_H

#include "internal/ucn_admission.h"
#include "internal/ucn_capability.h"
#include "internal/ucn_cluster.h"
#include "internal/ucn_coordinator.h"
#include "internal/ucn_flow.h"
#include "internal/ucn_group.h"
#include "internal/ucn_identity.h"
#include "internal/ucn_realtime.h"
#include "internal/ucn_route.h"
#include "internal/ucn_security.h"
#include "internal/ucn_service.h"
#include "internal/ucn_transport.h"
#include "ucn/ucn_core.h"
#include "ucn/ucn_persistence.h"

/* EN: IMPL-09 first freezes the product-composition contract without making
 * private Owners public.  Module and capability masks are deliberately
 * separate: compiling a module does not authorize or start a capability.
 * 中文：IMPL-09 首先冻结产品装配合同，但不把私有 Owner 暴露为公共接口。模块掩码
 * 与能力掩码有意分离：编译某模块不等于授权或启动对应能力。 */

#define UCN_I_COMPOSITION_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_RUNTIME_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_ADAPTER_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_LIFECYCLE_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_INIT_ORDER_CAPACITY 12U
#define UCN_I_COMPOSITION_REGISTRY_CAPACITY 14U
#define UCN_I_COMPOSITION_STORAGE_ALIGNMENT 8U

typedef uint32_t ucn_i_composition_module_mask_t;
enum {
    UCN_I_COMPOSE_FOUNDATION = UINT32_C(1) << 0,
    UCN_I_COMPOSE_PERSISTENCE = UINT32_C(1) << 1,
    UCN_I_COMPOSE_IDENTITY = UINT32_C(1) << 2,
    UCN_I_COMPOSE_SECURITY = UINT32_C(1) << 3,
    UCN_I_COMPOSE_ADMISSION = UINT32_C(1) << 4,
    UCN_I_COMPOSE_CAPABILITY = UINT32_C(1) << 5,
    UCN_I_COMPOSE_ROUTE = UINT32_C(1) << 6,
    UCN_I_COMPOSE_FLOW = UINT32_C(1) << 7,
    UCN_I_COMPOSE_TRANSPORT = UINT32_C(1) << 8,
    UCN_I_COMPOSE_SERVICE = UINT32_C(1) << 9,
    UCN_I_COMPOSE_REALTIME = UINT32_C(1) << 10,
    UCN_I_COMPOSE_GROUP = UINT32_C(1) << 11,
    UCN_I_COMPOSE_CLUSTER = UINT32_C(1) << 12
};

#define UCN_I_COMPOSE_KNOWN_MODULES                                      \
    (UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |              \
     UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |                   \
     UCN_I_COMPOSE_ADMISSION | UCN_I_COMPOSE_CAPABILITY |                \
     UCN_I_COMPOSE_ROUTE | UCN_I_COMPOSE_FLOW |                          \
     UCN_I_COMPOSE_TRANSPORT | UCN_I_COMPOSE_SERVICE |                   \
     UCN_I_COMPOSE_REALTIME | UCN_I_COMPOSE_GROUP |                      \
     UCN_I_COMPOSE_CLUSTER)

#ifndef UCN_V6S_FEATURE_REALTIME_ENABLED
#define UCN_V6S_FEATURE_REALTIME_ENABLED 0
#endif
#ifndef UCN_V6S_FEATURE_ADAPTER_ENABLED
#define UCN_V6S_FEATURE_ADAPTER_ENABLED 0
#endif
#ifndef UCN_V6S_FEATURE_GROUP_ENABLED
#define UCN_V6S_FEATURE_GROUP_ENABLED 0
#endif
#ifndef UCN_V6S_FEATURE_CLUSTER_ENABLED
#define UCN_V6S_FEATURE_CLUSTER_ENABLED 0
#endif

#if UCN_V6S_FEATURE_ADAPTER_ENABLED
#define UCN_I_COMPOSE_COMPILED_FOUNDATION UCN_I_COMPOSE_FOUNDATION
#else
#define UCN_I_COMPOSE_COMPILED_FOUNDATION UINT32_C(0)
#endif
#if UCN_FEATURE_PERSISTENCE_ENABLED
#define UCN_I_COMPOSE_COMPILED_PERSISTENCE UCN_I_COMPOSE_PERSISTENCE
#else
#define UCN_I_COMPOSE_COMPILED_PERSISTENCE UINT32_C(0)
#endif
#if UCN_V6S_FEATURE_REALTIME_ENABLED
#define UCN_I_COMPOSE_COMPILED_REALTIME UCN_I_COMPOSE_REALTIME
#else
#define UCN_I_COMPOSE_COMPILED_REALTIME UINT32_C(0)
#endif
#if UCN_V6S_FEATURE_GROUP_ENABLED
#define UCN_I_COMPOSE_COMPILED_GROUP UCN_I_COMPOSE_GROUP
#else
#define UCN_I_COMPOSE_COMPILED_GROUP UINT32_C(0)
#endif
#if UCN_V6S_FEATURE_CLUSTER_ENABLED
#define UCN_I_COMPOSE_COMPILED_CLUSTER UCN_I_COMPOSE_CLUSTER
#else
#define UCN_I_COMPOSE_COMPILED_CLUSTER UINT32_C(0)
#endif

/* Detailed IMPL-03..07 targets are always compiled for the private Host
 * integration build. Optional modules and Persistence remain physical build
 * choices. / IMPL-03..07 在私有 Host 集成构建中始终编译；可选模块与
 * Persistence 仍由真实构建选项决定。 */
#define UCN_I_COMPOSITION_COMPILED_MODULE_MASK                            \
    (UCN_I_COMPOSE_COMPILED_FOUNDATION | UCN_I_COMPOSE_IDENTITY |         \
     UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_ADMISSION |                   \
     UCN_I_COMPOSE_CAPABILITY | UCN_I_COMPOSE_ROUTE |                     \
     UCN_I_COMPOSE_FLOW | UCN_I_COMPOSE_TRANSPORT |                       \
     UCN_I_COMPOSE_SERVICE | UCN_I_COMPOSE_COMPILED_PERSISTENCE |         \
     UCN_I_COMPOSE_COMPILED_REALTIME | UCN_I_COMPOSE_COMPILED_GROUP |     \
     UCN_I_COMPOSE_COMPILED_CLUSTER)

typedef uint32_t ucn_i_composition_capability_mask_t;
enum {
    UCN_I_CAP_STATIC_C1 = UINT32_C(1) << 0,
    UCN_I_CAP_PROTECTED_C1 = UINT32_C(1) << 1,
    UCN_I_CAP_DYNAMIC_ADMISSION = UINT32_C(1) << 2,
    UCN_I_CAP_AUTO_ROUTE = UINT32_C(1) << 3,
    UCN_I_CAP_ADVANCED_FLOW = UINT32_C(1) << 4,
    UCN_I_CAP_RELIABLE = UINT32_C(1) << 5,
    UCN_I_CAP_TRANSFER = UINT32_C(1) << 6,
    UCN_I_CAP_SERVICE = UINT32_C(1) << 7,
    UCN_I_CAP_DURABLE_OPERATION = UINT32_C(1) << 8,
    UCN_I_CAP_LOCAL_STAMP = UINT32_C(1) << 9,
    UCN_I_CAP_NETWORK_TIME = UINT32_C(1) << 10,
    UCN_I_CAP_STATIC_GROUP = UINT32_C(1) << 11,
    UCN_I_CAP_DYNAMIC_GROUP = UINT32_C(1) << 12,
    UCN_I_CAP_CLUSTER = UINT32_C(1) << 13,
    UCN_I_CAP_CLUSTER_GROUP_ACCELERATION = UINT32_C(1) << 14
};

#define UCN_I_COMPOSE_KNOWN_CAPABILITIES                                 \
    (UCN_I_CAP_STATIC_C1 | UCN_I_CAP_PROTECTED_C1 |                      \
     UCN_I_CAP_DYNAMIC_ADMISSION | UCN_I_CAP_AUTO_ROUTE |                \
     UCN_I_CAP_ADVANCED_FLOW | UCN_I_CAP_RELIABLE |                      \
     UCN_I_CAP_TRANSFER | UCN_I_CAP_SERVICE |                            \
     UCN_I_CAP_DURABLE_OPERATION | UCN_I_CAP_LOCAL_STAMP |               \
     UCN_I_CAP_NETWORK_TIME | UCN_I_CAP_STATIC_GROUP |                   \
     UCN_I_CAP_DYNAMIC_GROUP | UCN_I_CAP_CLUSTER |                       \
     UCN_I_CAP_CLUSTER_GROUP_ACCELERATION)

typedef uint8_t ucn_i_composition_module_id_t;
enum {
    UCN_I_COMPOSITION_MODULE_FOUNDATION = 0,
    UCN_I_COMPOSITION_MODULE_PERSISTENCE = 1,
    UCN_I_COMPOSITION_MODULE_IDENTITY = 2,
    UCN_I_COMPOSITION_MODULE_SECURITY = 3,
    UCN_I_COMPOSITION_MODULE_ADMISSION = 4,
    UCN_I_COMPOSITION_MODULE_CAPABILITY = 5,
    UCN_I_COMPOSITION_MODULE_ROUTE = 6,
    UCN_I_COMPOSITION_MODULE_FLOW = 7,
    UCN_I_COMPOSITION_MODULE_TRANSPORT = 8,
    UCN_I_COMPOSITION_MODULE_SERVICE = 9,
    UCN_I_COMPOSITION_MODULE_REALTIME = 10,
    UCN_I_COMPOSITION_MODULE_GROUP = 11,
    UCN_I_COMPOSITION_MODULE_CLUSTER = 12,
    UCN_I_COMPOSITION_MODULE_COORDINATOR = 13
};

typedef uint8_t ucn_i_composition_owner_state_t;
enum {
    UCN_I_COMPOSITION_OWNER_REGISTERED = 1,
    UCN_I_COMPOSITION_OWNER_RESERVED = 2,
    UCN_I_COMPOSITION_OWNER_INITIALIZED = 3,
    UCN_I_COMPOSITION_OWNER_ACTIVE = 4
};

typedef uint8_t ucn_i_composition_runtime_phase_t;
enum {
    UCN_I_COMPOSITION_PREPARED = 1,
    UCN_I_COMPOSITION_RELOADING = 2,
    UCN_I_COMPOSITION_OWNERS_PUBLISHED = 3,
    UCN_I_COMPOSITION_RUNNING = 4,
    UCN_I_COMPOSITION_STOPPING = 5,
    UCN_I_COMPOSITION_QUIESCENT = 6,
    UCN_I_COMPOSITION_FAULT = 7
};

typedef struct ucn_i_composition_request {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t profile;
    /* Binary/product availability, not a user-selected execution graph. */
    ucn_i_composition_module_mask_t available_module_mask;
    ucn_i_composition_capability_mask_t capability_mask;
    uint32_t reserved_zero;
} ucn_i_composition_request_t;

typedef struct ucn_i_composition_plan {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t profile;
    ucn_i_composition_module_mask_t available_module_mask;
    ucn_i_composition_module_mask_t compiled_module_mask;
    ucn_i_composition_module_mask_t selected_module_mask;
    ucn_i_composition_module_mask_t durable_module_mask;
    ucn_i_composition_capability_mask_t capability_mask;
    uint64_t composition_digest;
    uint32_t private_owner_bytes;
    uint8_t init_count;
    uint8_t init_order[UCN_I_COMPOSITION_INIT_ORDER_CAPACITY];
    uint8_t reserved_zero[3];
} ucn_i_composition_plan_t;

/* EN: A reference is valid only inside one Runtime lifetime. The caller must
 * allocate runtime_instance from a no-wrap, never-reused local high-water.
 * 中文：引用只在单次 Runtime 生命周期内有效；调用方必须从不回绕、不复用的
 * 本地高水位分配 runtime_instance。 */
typedef struct ucn_i_composition_owner_ref {
    uint32_t runtime_instance;
    uint16_t owner_instance;
    uint16_t generation;
    uint8_t module_id;
    uint8_t reserved_zero[3];
} ucn_i_composition_owner_ref_t;

typedef struct ucn_i_composition_owner_record {
    ucn_i_composition_owner_ref_t ref;
    ucn_i_composition_module_mask_t module_mask;
    uint32_t storage_offset;
    uint32_t storage_bytes;
    uint8_t state;
    uint8_t reserved_zero[3];
} ucn_i_composition_owner_record_t;

typedef struct ucn_i_composition_runtime {
    uint32_t magic;
    uint32_t runtime_instance;
    uint32_t storage_bytes;
    uint16_t schema;
    uint16_t owner_instance_base;
    uint8_t phase;
    uint8_t registry_count;
    uint8_t initialized_count;
    uint8_t persistence_ready;
    ucn_node_t *foundation_node;
    uint32_t foundation_storage_bytes;
    uint8_t reserved_zero[4];
    ucn_i_composition_plan_t plan;
    ucn_i_composition_owner_record_t
        registry[UCN_I_COMPOSITION_REGISTRY_CAPACITY];
    ucn_i_coordinator_t coordinator;
    ucn_i_callback_gate_t lifecycle_gate;
} ucn_i_composition_runtime_t;

typedef struct ucn_i_composition_prepare_config {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t runtime_instance;
    uint16_t owner_instance_base;
    uint16_t reserved_zero;
    ucn_i_lock_ops_t coordinator_lock;
} ucn_i_composition_prepare_config_t;

/* EN: Capability is the only detailed Owner whose existing init API does not
 * take a named config object.  Composition gives it an exact typed config so
 * the lifecycle dispatcher never accepts an untyped void pointer.
 * 中文：Capability 是现有详细 Owner 中唯一没有命名配置对象的初始化入口。
 * Composition 为它补充精确类型，生命周期分派不接受无类型 void 指针。 */
typedef struct ucn_i_composition_capability_config {
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint16_t owner_instance;
    uint16_t security_owner_instance;
    uint64_t discovery_lease_us;
    uint64_t capability_lease_us;
    ucn_i_lock_ops_t state_lock;
} ucn_i_composition_capability_config_t;

/* EN: Every selected Owner has exactly one typed initialization input.  The
 * pointed-to immutable tables, Provider contexts and lock contexts must live
 * until the Composition reaches QUIESCENT.  An unselected module requires a
 * NULL field, preventing a hidden Owner from being initialized outside the
 * frozen Plan.
 * 中文：每个已选 Owner 只有一个强类型初始化输入。其不可变表、Provider 上下文
 * 与锁上下文必须存活到 Composition 进入 QUIESCENT；未选模块对应字段必须为
 * NULL，防止绕过冻结 Plan 偷偷初始化 Owner。 */
typedef struct ucn_i_composition_start_config {
    uint16_t struct_size;
    uint16_t schema;
    void *foundation_storage;
    size_t foundation_storage_bytes;
    const ucn_config_t *foundation_config;
    const ucn_ports_t *foundation_ports;
    const ucn_persistence_config_t *persistence;
    const ucn_i_identity_config_t *identity;
    const ucn_i_security_config_t *security;
    const ucn_i_admission_config_t *admission;
    const ucn_i_composition_capability_config_t *capability;
    const ucn_i_route_config_t *route;
    const ucn_i_lock_ops_t *route_state_lock;
    const ucn_i_flow_config_t *flow;
    const ucn_i_lock_ops_t *flow_state_lock;
    const ucn_i_transport_config_t *transport;
    const ucn_i_service_config_t *service;
    const ucn_i_realtime_config_t *realtime;
    const ucn_i_group_config_t *group;
    const ucn_i_cluster_config_t *cluster;
    uint32_t reserved_zero;
} ucn_i_composition_start_config_t;

typedef struct ucn_i_composition_lifecycle_view {
    uint16_t struct_size;
    uint16_t schema;
    uint16_t operations;
    uint8_t phase;
    uint8_t initialized_count;
    uint8_t active_owner_count;
    uint8_t persistence_ready;
    uint8_t foundation_ready;
    uint8_t made_progress;
    uint8_t reserved_zero[2];
} ucn_i_composition_lifecycle_view_t;

typedef struct ucn_i_composition_owner_view {
    ucn_i_composition_owner_ref_t ref;
    ucn_i_composition_module_mask_t module_mask;
    uint32_t storage_offset;
    uint32_t storage_bytes;
    uint8_t state;
    uint8_t reserved_zero[3];
} ucn_i_composition_owner_view_t;

/* EN: The typed adapter freezes both ends of one Coordinator dependency edge.
 * It contains no function pointer and cannot bypass Coordinator routing.
 * 中文：typed adapter 冻结一条 Coordinator 依赖边的请求者与目标；它不含函数
 * 指针，也不能绕过 Coordinator 路由。 */
typedef struct ucn_i_composition_typed_adapter {
    uint16_t struct_size;
    uint16_t schema;
    uint64_t composition_digest;
    ucn_i_composition_owner_ref_t requester;
    ucn_i_composition_owner_ref_t target;
    uint8_t dependency_kind;
    uint8_t reserved_zero[7];
} ucn_i_composition_typed_adapter_t;

UCN_STATIC_ASSERT(sizeof(ucn_i_composition_request_t) == 20U,
                  composition_request_must_be_20_bytes);
UCN_STATIC_ASSERT(sizeof(ucn_i_composition_plan_t) == 64U,
                  composition_plan_must_be_64_bytes);
UCN_STATIC_ASSERT(sizeof(ucn_i_composition_owner_ref_t) == 12U,
                  composition_owner_ref_must_be_12_bytes);
UCN_STATIC_ASSERT(sizeof(ucn_i_composition_owner_record_t) == 28U,
                  composition_owner_record_must_be_28_bytes);
UCN_STATIC_ASSERT(sizeof(ucn_i_composition_lifecycle_view_t) == 14U,
                  composition_lifecycle_view_must_be_14_bytes);
UCN_STATIC_ASSERT(UCN_I_COMPOSITION_REGISTRY_CAPACITY ==
                      UCN_I_COMPOSITION_MODULE_COORDINATOR + 1U,
                  composition_registry_must_cover_every_owner);

/* EN: The builder is a pure, zero-side-effect preflight.  It rejects missing
 * dependency closure before any Owner storage is initialized.
 * 中文：构建器是无副作用的纯预检；任何依赖闭包缺失都会在初始化 Owner Storage 前拒绝。 */
ucn_result_t ucn_i_composition_plan_build(
    const ucn_i_composition_request_t *request,
    ucn_i_composition_plan_t *plan_out);

bool ucn_i_composition_plan_valid(const ucn_i_composition_plan_t *plan);

ucn_result_t ucn_i_composition_storage_required(
    const ucn_i_composition_plan_t *plan,
    size_t *storage_bytes_out);
ucn_result_t ucn_i_composition_prepare(
    void *storage,
    size_t storage_bytes,
    const ucn_i_composition_plan_t *plan,
    const ucn_i_composition_prepare_config_t *config,
    ucn_i_composition_runtime_t **runtime_out);
bool ucn_i_composition_runtime_valid(
    const ucn_i_composition_runtime_t *runtime);
ucn_result_t ucn_i_composition_owner_view(
    const ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id,
    ucn_i_composition_owner_view_t *view_out);
ucn_result_t ucn_i_composition_typed_adapter_build(
    const ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_owner_ref_t *requester,
    ucn_i_dependency_kind_t dependency_kind,
    ucn_i_composition_typed_adapter_t *adapter_out);
ucn_result_t ucn_i_composition_route_requirement(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_typed_adapter_t *adapter,
    const ucn_i_dependency_requirement_t *requirement,
    uint64_t now_us,
    ucn_handle_t *handle_out);
ucn_result_t ucn_i_composition_route_event(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_typed_adapter_t *adapter,
    const ucn_i_dependency_event_t *event,
    uint64_t now_us);
/* EN: begin performs only local initialization and starts durable recovery;
 * step is the sole bounded path that may publish Owners and start Adapter RX.
 * 中文：begin 只做本地初始化并启动 durable recovery；只有有界 step 能发布
 * Owner 并启动 Adapter RX。 */
ucn_result_t ucn_i_composition_start_begin(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_start_config_t *config);
ucn_result_t ucn_i_composition_start_step(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    uint16_t operation_budget,
    ucn_i_composition_lifecycle_view_t *view_out);
ucn_result_t ucn_i_composition_stop_begin(
    ucn_i_composition_runtime_t *runtime);
ucn_result_t ucn_i_composition_stop_step(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    uint16_t operation_budget,
    ucn_i_composition_lifecycle_view_t *view_out);
ucn_result_t ucn_i_composition_lifecycle_view(
    const ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_lifecycle_view_t *view_out);
ucn_result_t ucn_i_composition_destroy(
    ucn_i_composition_runtime_t *runtime);

#endif
