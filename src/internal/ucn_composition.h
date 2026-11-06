#ifndef UCN_COMPOSITION_H
#define UCN_COMPOSITION_H

#include "internal/ucn_admission.h"
#include "internal/ucn_adapter.h"
#include "internal/ucn_capability.h"
#include "internal/ucn_cluster.h"
#include "internal/ucn_coordinator.h"
#include "internal/ucn_flow.h"
#include "internal/ucn_group.h"
#include "internal/ucn_identity.h"
#include "internal/ucn_persistence.h"
#include "internal/ucn_realtime.h"
#include "internal/ucn_route.h"
#include "internal/ucn_security.h"
#include "internal/ucn_service.h"
#include "internal/ucn_transport.h"
#include "ucn/ucn_core.h"

/* EN: IMPL-09 first freezes the product-composition contract without making
 * private Owners public.  Module and capability masks are deliberately
 * separate: compiling a module does not authorize or start a capability.
 * 中文：IMPL-09 首先冻结产品装配合同，但不把私有 Owner 暴露为公共接口。模块掩码
 * 与能力掩码有意分离：编译某模块不等于授权或启动对应能力。 */

#define UCN_I_COMPOSITION_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_RUNTIME_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_ADAPTER_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_LIFECYCLE_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_ADVANCED_USE_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_DELIVERY_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_CONTROL_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_ROUTE_CONTROL_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_CAPABILITY_CONTROL_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_TRANSPORT_CONTROL_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_SERVICE_CONTROL_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_DIAGNOSTIC_CONTROL_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_BOOTSTRAP_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_INGRESS_SCHEMA UINT16_C(1)
#define UCN_I_COMPOSITION_INIT_ORDER_CAPACITY 12U
#define UCN_I_COMPOSITION_REGISTRY_CAPACITY 14U
#define UCN_I_COMPOSITION_STORAGE_ALIGNMENT 8U
#define UCN_I_COMPOSITION_INGRESS_CLASS_COUNT 3U

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
    uint8_t ingress_cursor;
    uint8_t ingress_pending_kind;
    uint32_t ingress_generation;
    ucn_driver_token_t ingress_pending_token;
    ucn_node_t *foundation_node;
    uint32_t foundation_storage_bytes;
    uint8_t reserved_zero[4];
#if UCN_FEATURE_PERSISTENCE_ENABLED
    /* Lifecycle steps are mutually exclusive under lifecycle_gate.
     * 生命周期步骤受同一门控，复用有界暂存而非叠加两个Owner的栈。 */
    union {
        ucn_persistence_step_result_t persistence;
        struct {
            ucn_step_budget_t budget;
            ucn_step_result_t result;
        } foundation;
    } lifecycle_step;
#endif
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

/* EN: IMPL-09-04 accepts only one complete, current-use request.  Identity,
 * Admission, Security, Capability, Route and Flow references are never
 * interchangeable and a cached result is never accepted as authority.
 * 中文：IMPL-09-04 只接受一份完整的当前使用请求。Identity、Admission、
 * Security、Capability、Route 与 Flow 引用不能互换，缓存结果也绝不能替代
 * 当前 Owner 授权。 */
typedef struct ucn_i_composition_advanced_use_request {
    uint16_t struct_size;
    uint16_t schema;
    uint64_t now_us;
    ucn_i_identity_handle_t identity;
    ucn_i_admission_handle_t admission;
    ucn_i_security_access_request_t security;
    ucn_i_security_current_facts_t security_facts;
    ucn_i_capability_ref_t capability;
    ucn_i_route_domain_t route_domain;
    ucn_handle_t flow;
    ucn_i_flow_tx_request_t flow_request;
    uint32_t required_feature_bits;
    uint32_t route_link_generation;
    uint16_t required_path_capability_bits;
    uint16_t minimum_payload_bytes;
    uint32_t reserved_zero;
} ucn_i_composition_advanced_use_request_t;

typedef struct ucn_i_composition_advanced_use_plan {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t runtime_instance;
    uint64_t effective_deadline_us;
    ucn_handle_t flow;
    ucn_i_flow_tx_plan_t flow_plan;
    uint32_t identity_binding_generation;
    uint32_t admission_generation;
    uint32_t security_session_generation;
    uint32_t capability_generation;
    uint32_t route_generation;
    uint8_t route_kind;
    uint8_t reserved_zero[3];
} ucn_i_composition_advanced_use_plan_t;

/* EN: Scratch is caller-owned so the MCU call path never creates full Owner
 * views or a candidate Plan on its stack.  Its contents are diagnostic only
 * and grant no right.
 * 中文：Scratch 由调用方持有，避免 MCU 调用路径在栈上创建完整 Owner View 或
 * 候选 Plan；其中内容仅供诊断，不产生任何权限。 */
typedef struct ucn_i_composition_advanced_use_workspace {
    ucn_i_identity_binding_view_t identity;
    ucn_i_admitted_view_t admission;
    ucn_i_security_session_view_t security;
    ucn_i_cached_capability_t capability;
    ucn_i_route_resolved_t route;
    ucn_i_route_use_facts_t route_facts;
    ucn_i_flow_current_facts_t flow_facts;
    ucn_i_flow_tx_plan_t flow_plan;
    ucn_i_composition_advanced_use_plan_t candidate_plan;
} ucn_i_composition_advanced_use_workspace_t;

/* EN: Delivery and Interaction are independent dimensions.  NONE is valid
 * only for a volatile Service Request whose eventual payload remains owned by
 * the single Runtime Attempt created in IMPL-09-07.  Reliable and Transfer
 * reserve their exact private state here; Durable Operation is deliberately
 * excluded and uses the separate persistence requirement API below.
 * 中文：Delivery 与 Interaction 是两个独立维度。NONE 仅用于易失 Service
 * Request，其最终 Payload 仍由 IMPL-09-07 创建的唯一 Runtime Attempt 持有；
 * Reliable 与 Transfer 在此预留精确私有状态。Durable Operation 有意不混入
 * 本事务，而走下方独立的持久化 Requirement API。 */
typedef uint8_t ucn_i_composition_transport_mode_t;
enum {
    UCN_I_COMPOSITION_TRANSPORT_NONE = 0,
    UCN_I_COMPOSITION_TRANSPORT_RELIABLE = 1,
    UCN_I_COMPOSITION_TRANSPORT_TRANSFER = 2
};

typedef struct ucn_i_composition_delivery_request {
    uint16_t struct_size;
    uint16_t schema;
    uint64_t operation_id;
    uint64_t request_deadline_us;
    ucn_i_transport_transfer_setup_t transfer_template;
    uint8_t aad_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    uint8_t payload_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    uint8_t transport_mode;
    uint8_t reserved_zero[7];
} ucn_i_composition_delivery_request_t;

typedef struct ucn_i_composition_delivery_plan {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t runtime_instance;
    ucn_i_composition_advanced_use_plan_t use;
    ucn_handle_t transport;
    ucn_handle_t service;
    ucn_handle_t qos;
    uint64_t queue_deadline_us;
    uint8_t transport_mode;
    uint8_t interaction;
    uint8_t service_reserved;
    uint8_t reserved_zero[5];
} ucn_i_composition_delivery_plan_t;

/* EN: All large candidates are caller-owned so preparation does not copy a
 * Transport/Service transaction or Advanced Owner views onto an MCU stack.
 * 中文：所有大型候选均由调用方持有，prepare 不会在 MCU 栈上复制
 * Transport/Service 事务或 Advanced Owner View。 */
typedef struct ucn_i_composition_delivery_workspace {
    /* Transient pointers, valid only during the exact gated prepare call.
     * 仅本次受门控prepare调用期间有效，不是Owner存活/授权证明。 */
    ucn_i_transport_owner_t *transport_owner;
    ucn_i_service_owner_t *service_owner;
    ucn_i_flow_owner_t *flow_owner;
    ucn_i_composition_advanced_use_workspace_t use;
    ucn_i_transport_reliable_key_t reliable_key;
    ucn_i_transport_security_facts_t transport_security;
    ucn_i_transport_transfer_setup_t transfer_setup;
    ucn_i_transport_transfer_facts_t transfer_facts;
    union {
        ucn_i_transport_reliable_view_t reliable;
        ucn_i_transport_transfer_view_t transfer;
    } transport_view;
    ucn_i_service_key_t service_key;
    ucn_i_service_qos_item_t qos_item;
    ucn_i_composition_delivery_plan_t candidate_plan;
} ucn_i_composition_delivery_workspace_t;

typedef struct ucn_i_composition_delivery_cancel_workspace {
    ucn_i_service_qos_view_t qos;
    ucn_i_service_request_view_t service;
    union {
        ucn_i_transport_reliable_view_t reliable;
        ucn_i_transport_transfer_view_t transfer;
    } transport;
} ucn_i_composition_delivery_cancel_workspace_t;

typedef struct ucn_i_composition_durable_operation_workspace {
    ucn_i_service_operation_requirement_t requirement;
    ucn_handle_t operation;
} ucn_i_composition_durable_operation_workspace_t;

/* EN: Control dispatch never hands a raw Driver buffer to a business Owner.
 * Security first publishes an authenticated fact with a pending replay
 * reservation; Composition then freezes the exact target Owner and body.
 * 中文：Control dispatch 绝不把裸 Driver 缓冲交给业务 Owner。Security 必须先
 * 发布带待提交 Replay 预留的认证事实，Composition 再冻结精确目标 Owner 与
 * Body。 */
typedef struct ucn_i_composition_control_dispatch_plan {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t runtime_instance;
    ucn_i_composition_owner_ref_t target;
    ucn_i_security_authenticated_control_t authenticated;
    uint8_t opcode_domain;
    uint8_t target_module;
    uint8_t reserved_zero[2];
} ucn_i_composition_control_dispatch_plan_t;

/* EN: The caller supplies fixed staging for the complete C0 receive boundary.
 * Plaintext remains private until Security verification and module selection
 * both succeed; a Feature-OFF rejection therefore cannot expose business
 * bytes or retain a Replay reservation.
 * 中文：调用方为完整 C0 接收边界提供固定暂存。只有 Security 验证与目标模块
 * 选择都成功后才发布明文；因此 Feature-OFF 拒绝既不会暴露业务字节，也不会
 * 遗留 Replay 预留。 */
typedef struct ucn_i_composition_control_workspace {
    ucn_i_security_c0_path_workspace_t security;
    ucn_i_security_control_validate_workspace_t validate;
    ucn_i_security_authenticated_control_t authenticated;
    ucn_i_composition_control_dispatch_plan_t plan;
    uint8_t plaintext[UCN_ADAPTER_FRAME_BYTES];
} ucn_i_composition_control_workspace_t;

/* EN: A production C0 RX obligation stays claimed until the exact typed
 * module result commits or aborts Security replay.  The Driver buffer never
 * escapes this private plan and Adapter retirement is tied to the same token.
 * 中文：生产 C0 RX obligation 在精确 typed 模块结果提交或撤销 Security
 * Replay 前始终保持 CLAIMED。Driver Buffer 不会逃逸该私有 Plan，Adapter
 * 退休也绑定同一个 Token。 */
typedef struct ucn_i_composition_control_rx_plan {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t runtime_instance;
    ucn_i_composition_control_dispatch_plan_t dispatch;
    ucn_i_security_current_facts_t facts;
    ucn_i_security_current_facts_t hop_facts;
    ucn_i_security_hop_replay_handle_t hop_replay;
    ucn_driver_token_t rx_token;
    uint32_t link_instance_generation;
    uint16_t link_slot;
    uint8_t has_hop_replay;
    uint8_t reserved_zero;
} ucn_i_composition_control_rx_plan_t;

typedef struct ucn_i_composition_control_rx_workspace {
    ucn_i_composition_control_workspace_t control;
    ucn_i_security_c0_origin_resolve_workspace_t origin_resolve;
    ucn_i_security_control_access_request_t access;
    ucn_i_security_current_facts_t facts;
    ucn_i_security_handle_t hop_session;
    ucn_i_security_current_facts_t hop_facts;
    ucn_i_security_hop_replay_handle_t hop_replay;
    ucn_i_adapter_rx_view_t rx;
    ucn_i_composition_control_rx_plan_t candidate;
    struct {
        ucn_i_security_owner_t *security_owner;
        uint8_t *body_output;
        size_t *candidate_bytes_out;
        size_t body_capacity;
        size_t candidate_bytes;
        uint32_t current_link_instance;
        uint16_t current_link_mtu;
        bool link_ready;
        bool direct_context;
        bool has_hop;
        bool replay_pending;
        bool hop_replay_pending;
        bool claim_rx;
        uint8_t reserved_zero[2];
    } prepare;
} ucn_i_composition_control_rx_workspace_t;

typedef uint8_t ucn_i_composition_route_control_kind_t;
enum {
    UCN_I_COMPOSITION_ROUTE_CONTROL_RREQ = 1,
    UCN_I_COMPOSITION_ROUTE_CONTROL_RREP = 2,
    UCN_I_COMPOSITION_ROUTE_CONTROL_RERR = 3
};

/* EN: Route consumes only an already authenticated C0 fact.  The ingress
 * link is a separate hop-local fact and must match the current Security
 * session; it is never reconstructed from the logical C0 source address.
 * 中文：Route 只消费已经认证的 C0 Fact。Ingress Link 是独立的逐跳本地事实，
 * 必须与当前 Security Session 精确匹配，绝不能从逻辑 C0 Source 地址猜测。 */
typedef struct ucn_i_composition_route_control_result {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t runtime_instance;
    uint64_t transaction_id;
    uint16_t opcode;
    uint8_t kind;
    uint8_t invalidated;
    uint32_t reporter_route_generation;
    union {
        ucn_i_route_request_action_t request;
        ucn_i_route_reply_action_t reply;
    } action;
} ucn_i_composition_route_control_result_t;

typedef struct ucn_i_composition_route_control_workspace {
    ucn_i_security_control_validate_workspace_t validate;
    union {
        ucn_i_route_rreq_message_t rreq;
        ucn_i_route_rrep_message_t rrep;
        ucn_i_route_rerr_payload_t rerr;
    } message;
    ucn_i_composition_route_control_result_t candidate;
} ucn_i_composition_route_control_workspace_t;

typedef uint8_t ucn_i_composition_capability_control_kind_t;
enum {
    UCN_I_COMPOSITION_CAPABILITY_CONTROL_NEIGHBOR_HELLO = 1,
    UCN_I_COMPOSITION_CAPABILITY_CONTROL_NEIGHBOR_KEEPALIVE = 2,
    UCN_I_COMPOSITION_CAPABILITY_CONTROL_ADVERTISE = 3,
    UCN_I_COMPOSITION_CAPABILITY_CONTROL_SELECT = 4,
    UCN_I_COMPOSITION_CAPABILITY_CONTROL_ACK = 5,
    UCN_I_COMPOSITION_CAPABILITY_CONTROL_REVOKE = 6,
    UCN_I_COMPOSITION_CAPABILITY_CONTROL_QUERY = 7
};

typedef struct ucn_i_composition_capability_control_context {
    uint16_t struct_size;
    uint16_t schema;
    ucn_i_capability_record_t local_record;
    ucn_i_profile_requirements_t requirements;
    ucn_i_profile_select_t expected_select;
    uint8_t expected_transcript_digest[UCN_I_CAPABILITY_DIGEST_BYTES];
    uint8_t local_record_valid;
    uint8_t requirements_valid;
    uint8_t expected_select_valid;
    uint8_t expected_transcript_valid;
} ucn_i_composition_capability_control_context_t;

/* EN: Capability discovery is hop-local.  The handler therefore consumes the
 * complete RX plan that froze the Adapter Link slot/generation; callers may
 * not manufacture a second link identity beside that obligation.
 * 中文：Capability 发现属于逐跳语义，因此 Handler 直接消费已经冻结 Adapter
 * Link 槽位/代际的完整 RX Plan；调用方不能在该 obligation 之外再伪造一份
 * Link 身份。 */
typedef struct ucn_i_composition_capability_control_result {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t runtime_instance;
    uint64_t transaction_id;
    uint16_t opcode;
    uint8_t kind;
    uint8_t summary_result;
    ucn_i_capability_ref_t reference;
    ucn_i_profile_select_t selected;
    ucn_i_profile_ack_t acknowledged;
    ucn_i_capability_query_t revoked;
    ucn_i_capability_query_t query;
} ucn_i_composition_capability_control_result_t;

typedef struct ucn_i_composition_capability_control_workspace {
    ucn_i_security_control_validate_workspace_t validate;
    ucn_i_security_session_view_t session;
    ucn_i_authenticated_peer_view_t peer;
    ucn_i_capability_record_t record;
    ucn_i_cached_capability_t cached;
    ucn_i_capability_summary_t summary;
    ucn_i_profile_select_t selected;
    ucn_i_profile_ack_t acknowledged;
    ucn_i_capability_query_t query;
    uint8_t local_digest[UCN_I_CAPABILITY_DIGEST_BYTES];
    ucn_i_composition_capability_control_result_t candidate;
} ucn_i_composition_capability_control_workspace_t;

typedef uint8_t ucn_i_composition_transport_control_kind_t;
enum {
    UCN_I_COMPOSITION_TRANSPORT_CONTROL_SETUP = 1,
    UCN_I_COMPOSITION_TRANSPORT_CONTROL_SETUP_ACK = 2,
    UCN_I_COMPOSITION_TRANSPORT_CONTROL_ABORT = 3,
    UCN_I_COMPOSITION_TRANSPORT_CONTROL_TERMINAL = 4
};

/* EN: Context contains only local capabilities that cannot be trusted from
 * Wire: the exact pre-existing handle/parent and policy fingerprint.
 * 中文：Context 只携带 Wire 不可自证的本地能力：精确既有 Handle/Parent 与
 * Policy Fingerprint。 */
typedef struct ucn_i_composition_transport_control_context {
    uint16_t struct_size;
    uint16_t schema;
    ucn_handle_t parent;
    ucn_handle_t transfer;
    uint8_t parent_fingerprint[UCN_I_TRANSPORT_DIGEST_BYTES];
    uint32_t transport_policy_generation;
    uint16_t receive_credit;
    uint8_t parent_valid;
    uint8_t transfer_valid;
} ucn_i_composition_transport_control_context_t;

typedef struct ucn_i_composition_transport_control_result {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t runtime_instance;
    uint64_t transaction_id;
    ucn_handle_t transfer;
    ucn_i_transport_transfer_setup_ack_t setup_ack;
    uint16_t opcode;
    uint8_t kind;
    uint8_t exact_duplicate;
} ucn_i_composition_transport_control_result_t;

typedef struct ucn_i_composition_transport_control_workspace {
    ucn_i_security_control_validate_workspace_t validate;
    ucn_i_security_session_view_t session;
    ucn_i_transport_transfer_facts_t facts;
    ucn_i_transport_transfer_setup_t setup;
    ucn_i_transport_transfer_setup_ack_t setup_ack;
    ucn_i_transport_transfer_abort_body_t abort_body;
    ucn_i_transport_transfer_terminal_body_t terminal_body;
    ucn_i_transport_transfer_key_t expected_key;
    ucn_i_transport_transfer_view_t view;
    ucn_i_composition_transport_control_result_t candidate;
} ucn_i_composition_transport_control_workspace_t;

typedef uint8_t ucn_i_composition_service_control_kind_t;
enum {
    UCN_I_COMPOSITION_SERVICE_CONTROL_QUERY = 1,
    UCN_I_COMPOSITION_SERVICE_CONTROL_RECEIPT = 2,
    UCN_I_COMPOSITION_SERVICE_CONTROL_CANCEL = 3
};

/* EN: A C0 Operation control may only address an already-known local
 * durable operation.  Persistence data used by CANCEL is caller-owned and
 * revalidated by the Service owner; Wire never supplies durability facts.
 * 中文：C0 Operation 控制只能指向本地已知 durable Operation。CANCEL 使用的
 * Persistence 数据由调用方持有并由 Service Owner 复验，Wire 绝不提供
 * durability 事实。 */
typedef struct ucn_i_composition_service_control_context {
    uint16_t struct_size;
    uint16_t schema;
    ucn_handle_t operation;
    ucn_i_service_operation_durability_t durability;
    uint8_t operation_valid;
    uint8_t durability_valid;
    uint8_t reserved_zero[6];
} ucn_i_composition_service_control_context_t;

typedef struct ucn_i_composition_service_control_result {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t runtime_instance;
    uint64_t transaction_id;
    ucn_i_service_operation_view_t view;
    ucn_i_service_operation_requirement_t requirement;
    uint16_t opcode;
    uint8_t kind;
    uint8_t persistence_required;
} ucn_i_composition_service_control_result_t;

typedef struct ucn_i_composition_service_control_workspace {
    ucn_i_security_control_validate_workspace_t validate;
    ucn_i_security_session_view_t session;
    ucn_i_service_operation_wire_key_t key;
    ucn_i_service_operation_receipt_body_t receipt;
    ucn_i_service_operation_cancel_body_t cancel;
    ucn_i_service_operation_view_t view;
    ucn_i_sha256_workspace_t digest_workspace;
    uint8_t result_canonical[7U + UCN_I_SERVICE_RESULT_BYTES];
    uint8_t result_digest[UCN_I_SERVICE_DIGEST_BYTES];
    ucn_i_composition_service_control_result_t candidate;
} ucn_i_composition_service_control_workspace_t;

typedef struct ucn_i_composition_diagnostic_control_result {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t runtime_instance;
    uint64_t transaction_id;
    uint64_t nonce;
    uint16_t opcode;
    uint16_t response_opcode;
} ucn_i_composition_diagnostic_control_result_t;

typedef struct ucn_i_composition_diagnostic_control_workspace {
    ucn_i_security_control_validate_workspace_t validate;
    ucn_i_diagnostic_echo_t echo;
    ucn_i_composition_diagnostic_control_result_t candidate;
} ucn_i_composition_diagnostic_control_workspace_t;

/* EN: The caller-owned TX workspace is the exact Adapter buffer obligation.
 * It must remain byte-identical until finish observes a terminal token; the
 * immutable digest in the plan detects mutation before retirement.
 * 中文：caller-owned TX workspace 就是精确 Adapter Buffer obligation。在
 * finish 观察到终态 Token 前必须逐字节不变；Plan 中的不可变摘要用于在退休前
 * 检测篡改。 */
typedef struct ucn_i_composition_control_tx_plan {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t runtime_instance;
    const uint8_t *frame;
    size_t frame_bytes;
    ucn_driver_token_t tx_token;
    uint8_t frame_digest[UCN_I_DIGEST128_BYTES];
    uint32_t link_instance_generation;
    uint16_t link_slot;
    uint8_t reserved_zero[2];
} ucn_i_composition_control_tx_plan_t;

typedef struct ucn_i_composition_control_tx_workspace {
    /* EN: One workspace serves both the direct-link fast path and the
     * independent Origin/Hop multi-hop path.  Direct protect uses packet;
     * path protect additionally uses the fixed origin_packet staging.
     * 中文：同一工作区同时服务直连快速路径与 Origin/Hop 独立的多跳路径。
     * 直连只使用 packet；多跳还会使用固定的 origin_packet 暂存。 */
    ucn_i_security_c0_path_workspace_t security;
    ucn_i_adapter_tx_view_t adapter;
    ucn_i_sha256_workspace_t digest_workspace;
    ucn_i_composition_control_tx_plan_t candidate;
    uint8_t frame[UCN_ADAPTER_FRAME_BYTES];
} ucn_i_composition_control_tx_workspace_t;

/* EN: O0 Bootstrap is intentionally not represented as an authenticated
 * Security fact.  Composition decodes one of the three frozen stateless
 * Admission messages into this closed typed union and freezes the exact
 * Admission Owner, ingress Link generation and Adapter RX obligation.
 * 中文：O0 Bootstrap 有意不伪装成已认证 Security Fact。Composition 只把三个
 * 已冻结的无状态 Admission 消息解码到该封闭类型联合，并冻结精确 Admission
 * Owner、入站 Link 代际与 Adapter RX obligation。 */
typedef union ucn_i_composition_bootstrap_message {
    ucn_i_admission_hello_t hello;
    ucn_i_admission_cookie_challenge_t cookie_challenge;
    ucn_i_admission_hello_cookie_t hello_cookie;
} ucn_i_composition_bootstrap_message_t;

typedef struct ucn_i_composition_bootstrap_rx_plan {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t runtime_instance;
    ucn_i_composition_owner_ref_t target;
    ucn_i_admission_link_t ingress_link;
    ucn_driver_token_t rx_token;
    uint64_t transaction_id;
    uint32_t realm_id;
    uint32_t link_instance_generation;
    uint16_t opcode;
    uint16_t link_slot;
    uint8_t interaction_role;
    uint8_t reserved_zero[3];
    uint8_t message_digest[UCN_I_DIGEST128_BYTES];
    ucn_i_composition_bootstrap_message_t message;
} ucn_i_composition_bootstrap_rx_plan_t;

typedef struct ucn_i_composition_bootstrap_rx_workspace {
    struct {
        uint32_t link_generation;
        uint16_t link_mtu;
        bool link_ready;
    } prepare;
    ucn_i_adapter_rx_view_t rx;
    ucn_i_c0_frame_t frame;
    ucn_i_sha256_workspace_t digest_workspace;
    ucn_i_composition_bootstrap_rx_plan_t candidate;
} ucn_i_composition_bootstrap_rx_workspace_t;

typedef struct ucn_i_composition_bootstrap_tx_workspace {
    ucn_i_adapter_tx_view_t adapter;
    ucn_i_sha256_workspace_t digest_workspace;
    ucn_i_admission_link_t current_link;
    ucn_i_c0_frame_t c0_frame;
    ucn_driver_token_t token;
    ucn_i_composition_control_tx_plan_t candidate;
    uint8_t body[UCN_I_ADMISSION_HELLO_COOKIE_MAX_BYTES];
    uint8_t frame[UCN_ADAPTER_FRAME_BYTES];
} ucn_i_composition_bootstrap_tx_workspace_t;

typedef uint8_t ucn_i_composition_ingress_kind_t;
enum {
    UCN_I_COMPOSITION_INGRESS_NONE = 0,
    UCN_I_COMPOSITION_INGRESS_FOUNDATION = 1,
    UCN_I_COMPOSITION_INGRESS_BOOTSTRAP = 2,
    UCN_I_COMPOSITION_INGRESS_AUTHENTICATED_CONTROL = 3,
    UCN_I_COMPOSITION_INGRESS_CONTROL_RELAY = 4
};

typedef struct ucn_i_composition_ingress_result {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t runtime_instance;
    uint32_t generation;
    uint16_t operations;
    uint8_t selected_kind;
    uint8_t more_work;
    uint8_t next_cursor;
    uint8_t reserved_zero[3];
} ucn_i_composition_ingress_result_t;

/* EN: The scheduler workspace owns every byte needed by one selected ingress
 * obligation.  A Bootstrap or authenticated Control result remains live in
 * this object until ingress_finish retires the exact Adapter token; callers
 * must not move, clear or reuse the workspace while active_kind is nonzero.
 * 中文：调度工作区拥有一条已选择 ingress obligation 所需的全部字节。
 * Bootstrap 或认证 Control 结果在 ingress_finish 退休精确 Adapter Token 前
 * 始终存活于本对象；active_kind 非零期间调用方不得移动、清零或复用工作区。 */
typedef struct ucn_i_composition_ingress_workspace {
    uint16_t struct_size;
    uint16_t schema;
    uint32_t runtime_instance;
    uint32_t generation;
    uint8_t active_kind;
    uint8_t reserved_zero[3];
    /* EN: Scheduler scratch is caller-owned so the synchronous Nano stack
     * does not grow with the Composition wrapper.  These fields are private
     * to ingress_step and are never accepted as authority on ingress_finish.
     * 中文：调度暂存由调用方提供，避免 Nano 同步栈随 Composition 包装层增长；
     * 这些字段只供 ingress_step 使用，ingress_finish 不把它们作为授权依据。 */
    ucn_i_callback_claim_t scheduler_claim;
    uint16_t scheduler_operations;
    uint8_t scheduler_inspected;
    uint8_t scheduler_selected_kind;
    uint8_t scheduler_foundation_more_work;
    uint8_t scheduler_reserved[3];
    size_t control_body_bytes;
    ucn_step_budget_t foundation_budget;
    ucn_step_result_t foundation_result;
    ucn_i_composition_ingress_result_t result_candidate;
    ucn_i_composition_bootstrap_rx_workspace_t bootstrap;
    ucn_i_composition_bootstrap_rx_plan_t bootstrap_plan;
    ucn_i_composition_control_rx_workspace_t control;
    ucn_i_composition_control_rx_plan_t control_plan;
    ucn_i_security_c0_relay_fact_t relay_fact;
    ucn_i_security_current_facts_t relay_inbound_facts;
    ucn_i_security_current_facts_t relay_outbound_facts;
    ucn_i_security_handle_t relay_outbound_session;
    ucn_i_route_forward_key_t relay_key;
    ucn_i_route_resolved_t relay_route;
    ucn_i_adapter_tx_view_t relay_tx_view;
    ucn_driver_token_t relay_tx_token;
    ucn_i_sha256_workspace_t relay_digest_workspace;
    uint8_t relay_frame_digest[UCN_I_DIGEST128_BYTES];
    size_t relay_frame_bytes;
    uint32_t relay_link_instance_generation;
    uint16_t relay_link_slot;
    uint8_t relay_tx_reserved;
    uint8_t reserved_relay;
    uint8_t control_body[UCN_ADAPTER_FRAME_BYTES];
} ucn_i_composition_ingress_workspace_t;

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

/* EN: The only IMPL-09-04 advanced-send preflight executes the immutable
 * order Identity/Admission -> Security/ACL -> Capability -> Route -> Flow.
 * Flow sequence reservation is last; plan_out is written only after every
 * current Owner has accepted the request.  The Plan is a short-lived local
 * reservation, not cached authority: a future raw-frame submit path must run
 * this preflight at its actual use point and finish only that exact Plan.
 * 中文：唯一的 IMPL-09-04 高级发送预检严格执行 Identity/Admission →
 * Security/ACL → Capability → Route → Flow；Flow Sequence 最后才预留，且
 * 只有所有当前 Owner 均接受后才写 plan_out。Plan 只是短生命周期的本地预留，
 * 不是可缓存 Authority；未来 raw-frame 提交路径必须在真实使用点执行本预检，并且
 * 只收口该次精确 Plan。 */
ucn_result_t ucn_i_composition_advanced_use_preflight(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_advanced_use_request_t *request,
    ucn_i_composition_advanced_use_workspace_t *workspace,
    ucn_i_composition_advanced_use_plan_t *plan_out);
ucn_result_t ucn_i_composition_advanced_use_finish(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_advanced_use_plan_t *plan,
    uint8_t delivered);

/* EN: IMPL-09-05 atomically reserves current Advanced use, optional volatile
 * Request correlation, exactly one Reliable/Transfer owner, and one Q1-Q3
 * scheduler reference.  Q0 and Latest are intentionally absent: Q0 requires
 * a typed control authority and ordinary Latest continues to use Foundation.
 * On any failure every reservation is rolled back and plan_out is untouched.
 * 中文：IMPL-09-05 原子预留当前 Advanced use、可选的易失 Request 关联、
 * 恰好一个 Reliable/Transfer Owner 以及一个 Q1～Q3 调度引用。此处有意不
 * 接受 Q0/Latest：Q0 需要强类型控制 Authority，普通 Latest 继续复用
 * Foundation。任一失败都会回滚全部预留，且不写 plan_out。 */
ucn_result_t ucn_i_composition_delivery_prepare(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_advanced_use_request_t *use_request,
    const ucn_i_composition_delivery_request_t *delivery_request,
    const uint8_t *body,
    size_t body_bytes,
    ucn_i_composition_delivery_workspace_t *workspace,
    ucn_i_composition_delivery_plan_t *plan_out);
ucn_result_t ucn_i_composition_delivery_cancel(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_delivery_plan_t *plan,
    ucn_i_composition_delivery_cancel_workspace_t *workspace);

/* EN: Durable Operation never enters QoS before PREPARED has been persisted
 * and reloaded.  This entry only creates the Service transition and returns
 * its immutable Persistence requirement; it is unavailable when Persistence
 * or the durable capability is absent.
 * 中文：Durable Operation 在 PREPARED 完成持久化并回读前绝不进入 QoS。
 * 本入口只创建 Service 转换并返回不可变 Persistence requirement；未选择
 * Persistence 或 durable capability 时不可用。 */
ucn_result_t ucn_i_composition_durable_operation_begin(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_service_operation_key_t *key,
    const ucn_i_service_operation_durability_t *durability,
    uint64_t now_us,
    ucn_i_composition_durable_operation_workspace_t *workspace,
    ucn_handle_t *operation_out,
    ucn_i_service_operation_requirement_t *requirement_out);

/* EN: This is the only raw-control-to-typed dispatch boundary. prepare is a
 * zero-business-side-effect preflight; the internal module handler consumes
 * the immutable plan and finish atomically commits or aborts its exact replay
 * reservation.  A raw decoded C0 frame has no API that can enter this path.
 * 中文：这是唯一 raw control → typed dispatch 边界。prepare 是无业务副作用
 * 预检；内部模块 handler 消费不可变 Plan 后，由 finish 精确提交或撤销其 Replay
 * 预留。裸 C0 解码结果没有进入本路径的 API。 */
ucn_result_t ucn_i_composition_control_dispatch_prepare(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_security_authenticated_control_t *authenticated,
    const ucn_i_security_current_facts_t *current_facts,
    ucn_i_security_control_validate_workspace_t *workspace,
    ucn_i_composition_control_dispatch_plan_t *plan_out);
ucn_result_t ucn_i_composition_control_dispatch_finish(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_dispatch_plan_t *plan,
    const ucn_i_security_current_facts_t *current_facts,
    ucn_i_security_control_validate_workspace_t *workspace,
    bool consumed);
ucn_result_t ucn_i_composition_control_protect(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_security_c0_protect_request_t *request,
    const uint8_t *plaintext,
    size_t plaintext_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *packet_output,
    size_t packet_capacity,
    size_t *packet_bytes);
ucn_result_t ucn_i_composition_control_receive_prepare(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_security_control_access_request_t *access,
    const ucn_i_security_current_facts_t *current_facts,
    const uint8_t *packet,
    size_t packet_bytes,
    ucn_i_composition_control_workspace_t *workspace,
    uint8_t *body_output,
    size_t body_capacity,
    size_t *body_bytes,
    ucn_i_composition_control_dispatch_plan_t *plan_out);
ucn_result_t ucn_i_composition_control_rx_claim_prepare(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    ucn_i_composition_control_rx_workspace_t *workspace,
    uint8_t *body_output,
    size_t body_capacity,
    size_t *body_bytes,
    ucn_i_composition_control_rx_plan_t *plan_out);
ucn_result_t ucn_i_composition_control_rx_finish(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_rx_plan_t *plan,
    ucn_i_security_control_validate_workspace_t *workspace,
    bool consumed);
/* EN: Advance the shared C0/C1 ingress with one compile-time-bounded pass.
 * The three classes are Foundation, O0 Bootstrap and O1/O2 authenticated
 * Control.  The persistent cursor advances even across an empty class, while
 * each class may consume at most one item per call.
 * 中文：以一次编译期有界扫描推进共享 C0/C1 入站。三个类别分别为 Foundation、
 * O0 Bootstrap 与 O1/O2 认证 Control；持久游标经过空类别时也会前移，且每次
 * 调用中每类最多消费一项。 */
ucn_result_t ucn_i_composition_ingress_step(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    const ucn_step_budget_t *budget,
    ucn_i_composition_ingress_workspace_t *workspace,
    ucn_i_composition_ingress_result_t *result_out);
ucn_result_t ucn_i_composition_ingress_finish(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_ingress_workspace_t *workspace,
    bool consumed);
/* EN: Decode and apply exactly one authenticated, direct Route control
 * message.  Replay remains pending; the caller must finish the original RX
 * plan with consumed=true only after accepting this typed result.  Current
 * Security sessions bind one logical peer and one ingress Link, so relay
 * forwarding is intentionally not claimed by this entry yet.
 * 中文：解码并应用一条已认证的直达 Route 控制消息。Replay 仍保持待提交；调用方
 * 只有接受该 typed 结果后，才能以 consumed=true 完成原 RX Plan。当前 Security
 * Session 只绑定一个逻辑 Peer 与一个 Ingress Link，因此本入口尚不宣称多跳中继。 */
ucn_result_t ucn_i_composition_route_control_handle(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_dispatch_plan_t *plan,
    const ucn_i_security_current_facts_t *current_facts,
    const ucn_i_route_link_ref_t *ingress,
    uint64_t now_us,
    ucn_i_composition_route_control_workspace_t *workspace,
    ucn_i_composition_route_control_result_t *result_out);
/* EN: Decode and apply one authenticated CAPABILITY_ADVERTISE received from
 * the production Adapter path.  The caller must finish the same RX plan with
 * consumed=true only after this handler succeeds.
 * 中文：解码并应用一条经生产 Adapter 路径接收、已认证的
 * CAPABILITY_ADVERTISE。只有该 Handler 成功后，调用方才可对同一 RX Plan
 * 以 consumed=true 完成 Replay 与 Buffer 退休。 */
ucn_result_t ucn_i_composition_capability_control_handle(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_rx_plan_t *rx_plan,
    uint64_t now_us,
    ucn_i_composition_capability_control_workspace_t *workspace,
    ucn_i_composition_capability_control_result_t *result_out);
ucn_result_t ucn_i_composition_capability_control_handle_with_context(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_rx_plan_t *rx_plan,
    uint64_t now_us,
    const ucn_i_composition_capability_control_context_t *context,
    ucn_i_composition_capability_control_workspace_t *workspace,
    ucn_i_composition_capability_control_result_t *result_out);
ucn_result_t ucn_i_composition_transport_control_handle(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_rx_plan_t *rx_plan,
    uint64_t now_us,
    const ucn_i_composition_transport_control_context_t *context,
    ucn_i_composition_transport_control_workspace_t *workspace,
    ucn_i_composition_transport_control_result_t *result_out);
ucn_result_t ucn_i_composition_service_control_handle(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_rx_plan_t *rx_plan,
    uint64_t now_us,
    const ucn_i_composition_service_control_context_t *context,
    ucn_i_composition_service_control_workspace_t *workspace,
    ucn_i_composition_service_control_result_t *result_out);
ucn_result_t ucn_i_composition_diagnostic_control_handle(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_dispatch_plan_t *plan,
    const ucn_i_security_current_facts_t *current_facts,
    ucn_i_composition_diagnostic_control_workspace_t *workspace,
    ucn_i_composition_diagnostic_control_result_t *result_out);
ucn_result_t ucn_i_composition_control_tx_submit(
    ucn_i_composition_runtime_t *runtime,
    uint16_t link_slot,
    const ucn_i_security_c0_protect_request_t *request,
    const uint8_t *body,
    size_t body_bytes,
    ucn_i_composition_control_tx_workspace_t *workspace,
    ucn_i_composition_control_tx_plan_t *plan_out);
/* EN: Submit one multi-hop C0 control frame with independent end-to-end and
 * first-hop Security sessions.  origin_request owns immutable Origin
 * protection; hop_session/hop_facts own only the selected physical Link.
 * 中文：使用彼此独立的端到端 Session 与首跳 Session 提交一帧多跳 C0
 * Control。origin_request 只负责不可变 Origin 保护；hop_session/hop_facts
 * 只负责所选物理 Link。 */
ucn_result_t ucn_i_composition_control_path_tx_submit(
    ucn_i_composition_runtime_t *runtime,
    uint16_t link_slot,
    const ucn_i_security_c0_protect_request_t *origin_request,
    ucn_i_security_handle_t hop_session,
    const ucn_i_security_current_facts_t *hop_facts,
    const uint8_t *body,
    size_t body_bytes,
    ucn_i_composition_control_tx_workspace_t *workspace,
    ucn_i_composition_control_tx_plan_t *plan_out);
/* EN: Retry a definitive Driver NOT_SUBMITTED outcome with the immutable
 * sealed frame.  Security is not entered and the C0 nonce is not recomputed.
 * 中文：对 Driver 明确 NOT_SUBMITTED 的结果复用不可变 sealed frame 重试；
 * 不重新进入 Security，也不重新计算 C0 nonce。 */
ucn_result_t ucn_i_composition_control_tx_retry(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_tx_plan_t *prior,
    ucn_i_composition_control_tx_workspace_t *workspace,
    ucn_i_composition_control_tx_plan_t *plan_out);
ucn_result_t ucn_i_composition_control_tx_finish(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_tx_plan_t *plan,
    ucn_i_sha256_workspace_t *workspace);

/* EN: This separate ingress path claims only C0/O0.  It never creates a
 * Security replay fact and it accepts only the three frozen stateless
 * Bootstrap body codecs.  Later Admission phases must use authenticated O1
 * or O2 dispatch once their exact Wire bodies are frozen.
 * 中文：该独立入口只认领 C0/O0，不创建 Security Replay Fact，并且只接受三个
 * 已冻结的无状态 Bootstrap Body Codec。后续 Admission 阶段须在其精确 Wire
 * Body 冻结后改走已认证 O1/O2 分派。 */
ucn_result_t ucn_i_composition_bootstrap_rx_claim_prepare(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_bootstrap_rx_workspace_t *workspace,
    ucn_i_composition_bootstrap_rx_plan_t *plan_out);
ucn_result_t ucn_i_composition_bootstrap_rx_finish(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_bootstrap_rx_plan_t *plan,
    ucn_i_sha256_workspace_t *workspace,
    bool consumed);
ucn_result_t ucn_i_composition_bootstrap_tx_submit(
    ucn_i_composition_runtime_t *runtime,
    uint16_t link_slot,
    uint16_t opcode,
    const ucn_i_composition_bootstrap_message_t *message,
    ucn_i_composition_bootstrap_tx_workspace_t *workspace,
    ucn_i_composition_control_tx_plan_t *plan_out);

/* EN: IMPL-09-06 keeps Optional Feature operations narrow and independent.
 * LOCAL_STAMP never reaches Flow/Persistence, Static Group never reaches
 * Cluster, and Cluster Base never reaches Realtime/Group.  These are private
 * Composition entry points; raw Wire dispatch remains IMPL-09-07.
 * 中文：IMPL-09-06 保持可选能力入口狭窄且相互独立。LOCAL_STAMP 不进入
 * Flow/Persistence，Static Group 不进入 Cluster，Cluster Base 不进入
 * Realtime/Group。这些仍是 Composition 私有入口；raw Wire 分派留给
 * IMPL-09-07。 */
#if UCN_V6S_FEATURE_REALTIME_ENABLED
ucn_result_t ucn_i_composition_realtime_local_policy_set(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_realtime_policy_t *policy);
ucn_result_t ucn_i_composition_realtime_local_prepare(
    ucn_i_composition_runtime_t *runtime, uint16_t endpoint,
    uint64_t local_capture_us, uint32_t sample_capture_bound_us,
    bool hardware_capture, ucn_i_realtime_prepared_t *prepared_out);
#endif

#if UCN_V6S_FEATURE_GROUP_ENABLED
ucn_result_t ucn_i_composition_group_install_static(
    ucn_i_composition_runtime_t *runtime, uint16_t static_slot,
    const ucn_i_group_context_config_t *config,
    bool manifest_authenticated, bool manifest_anti_rollback,
    ucn_handle_t *group_out);
ucn_result_t ucn_i_composition_group_send_begin(
    ucn_i_composition_runtime_t *runtime, ucn_handle_t group,
    const ucn_i_group_send_request_t *request, uint64_t now_us,
    ucn_handle_t *send_out);
ucn_result_t ucn_i_composition_group_attempt_peek(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_group_attempt_view_t *attempt_out);
#endif

#if UCN_V6S_FEATURE_CLUSTER_ENABLED
ucn_result_t ucn_i_composition_cluster_member_observe(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_cluster_member_fact_t *fact, uint64_t now_us);
ucn_result_t ucn_i_composition_cluster_authority_preflight(
    ucn_i_composition_runtime_t *runtime, uint64_t now_us,
    ucn_i_cluster_authority_view_t *view_out);
#endif

/* EN: Static C1 is routed through the Composition Coordinator facade but
 * reuses the single Foundation state machine.  These private APIs neither
 * expose the embedded node pointer nor create a parallel queue/sequence.
 * 中文：静态 C1 经 Composition Coordinator 外观路由，但复用唯一 Foundation
 * 状态机；这些私有入口既不暴露内嵌 node 指针，也不创建平行队列或 Sequence。 */
ucn_result_t ucn_i_composition_endpoint_add(
    ucn_i_composition_runtime_t *runtime,
    const ucn_endpoint_config_t *config,
    ucn_endpoint_handle_t *endpoint_out);
ucn_result_t ucn_i_composition_endpoint_remove(
    ucn_i_composition_runtime_t *runtime,
    ucn_endpoint_handle_t endpoint);
ucn_result_t ucn_i_composition_static_path_add(
    ucn_i_composition_runtime_t *runtime,
    const ucn_static_path_t *path,
    ucn_path_handle_t *path_out);
ucn_result_t ucn_i_composition_static_path_remove(
    ucn_i_composition_runtime_t *runtime,
    ucn_path_handle_t path);
ucn_result_t ucn_i_composition_link_get(
    ucn_i_composition_runtime_t *runtime,
    uint16_t link_index,
    ucn_link_handle_t *link_out);
ucn_result_t ucn_i_composition_publish(
    ucn_i_composition_runtime_t *runtime,
    const ucn_target_t *target,
    const void *payload,
    size_t payload_bytes,
    const ucn_send_options_t *options,
    ucn_send_handle_t *handle_out);
ucn_result_t ucn_i_composition_send_query(
    ucn_i_composition_runtime_t *runtime,
    ucn_send_handle_t handle,
    ucn_send_view_t *view_out);
ucn_result_t ucn_i_composition_send_cancel(
    ucn_i_composition_runtime_t *runtime,
    ucn_send_handle_t handle);
ucn_result_t ucn_i_composition_send_forget(
    ucn_i_composition_runtime_t *runtime,
    ucn_send_handle_t handle);
ucn_result_t ucn_i_composition_get_stats(
    ucn_i_composition_runtime_t *runtime,
    ucn_stats_t *stats_out);
ucn_result_t ucn_i_composition_step(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    const ucn_step_budget_t *budget,
    ucn_step_result_t *result_out);

/* EN: Only an exact Foundation callback scope may read its frozen snapshot.
 * Mutating Composition entry points remain closed while the step gate is held.
 * 中文：只有精确的 Foundation 回调 Scope 能读取其冻结快照；step 门持有期间，
 * Composition 的所有写入口继续关闭。 */
ucn_result_t ucn_i_composition_callback_link_get(
    const ucn_i_composition_runtime_t *runtime,
    ucn_callback_scope_t scope,
    uint16_t link_index,
    ucn_link_handle_t *link_out);
ucn_result_t ucn_i_composition_callback_send_query(
    const ucn_i_composition_runtime_t *runtime,
    ucn_callback_scope_t scope,
    ucn_send_handle_t handle,
    ucn_send_view_t *view_out);
ucn_result_t ucn_i_composition_callback_get_stats(
    const ucn_i_composition_runtime_t *runtime,
    ucn_callback_scope_t scope,
    ucn_stats_t *stats_out);

/* EN: Driver facts may arrive asynchronously or synchronously from inside
 * submit/cancel.  The implementation accepts only the exact active step/stop
 * transaction for nested completion and otherwise acquires the Coordinator
 * gate before reaching Foundation.
 * 中文：Driver Fact 既可能异步到达，也可能在 submit/cancel 内同步早到。实现只
 * 允许精确活动的 step/stop 事务接收嵌套完成；其他情况必须先取得 Coordinator 门。 */
ucn_result_t ucn_i_composition_driver_rx_publish(
    ucn_i_composition_runtime_t *runtime,
    ucn_link_handle_t link,
    const uint8_t *bytes,
    size_t length,
    const ucn_rx_meta_t *meta);
ucn_result_t ucn_i_composition_driver_tx_complete(
    ucn_i_composition_runtime_t *runtime,
    ucn_driver_token_t token,
    ucn_result_t result,
    const ucn_tx_meta_t *meta);
ucn_result_t ucn_i_composition_driver_link_event(
    ucn_i_composition_runtime_t *runtime,
    ucn_link_handle_t link,
    ucn_driver_link_event_t event,
    const ucn_link_event_meta_t *meta);

ucn_result_t ucn_i_composition_destroy(
    ucn_i_composition_runtime_t *runtime);

#endif
