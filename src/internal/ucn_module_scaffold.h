#ifndef UCN_MODULE_SCAFFOLD_H
#define UCN_MODULE_SCAFFOLD_H

#include "ucn/ucn_types.h"

#define UCN_I_MODULE_SCAFFOLD_SCHEMA UINT16_C(1)

typedef uint8_t ucn_i_module_id_t;
enum {
    UCN_I_MODULE_SECURITY = 0,
    UCN_I_MODULE_ADMISSION = 1,
    UCN_I_MODULE_ROUTING = 2,
    UCN_I_MODULE_TRANSPORT = 3,
    UCN_I_MODULE_SERVICE = 4,
    UCN_I_MODULE_REALTIME = 5,
    UCN_I_MODULE_GROUP = 6,
    UCN_I_MODULE_CLUSTER = 7,
    UCN_I_MODULE_COUNT = 8
};

typedef uint16_t ucn_i_module_dependency_mask_t;
enum {
    UCN_I_DEP_COMMON = UINT16_C(1) << 0,
    UCN_I_DEP_COORDINATOR = UINT16_C(1) << 1,
    UCN_I_DEP_PERSISTENCE = UINT16_C(1) << 2,
    UCN_I_DEP_SECURITY = UINT16_C(1) << 3,
    UCN_I_DEP_ADMISSION = UINT16_C(1) << 4,
    UCN_I_DEP_ROUTING = UINT16_C(1) << 5,
    UCN_I_DEP_TRANSPORT = UINT16_C(1) << 6,
    UCN_I_DEP_SERVICE = UINT16_C(1) << 7,
    UCN_I_DEP_REALTIME = UINT16_C(1) << 8,
    UCN_I_DEP_GROUP = UINT16_C(1) << 9,
    UCN_I_DEP_CLUSTER = UINT16_C(1) << 10
};

typedef uint16_t ucn_i_module_scaffold_flags_t;
enum {
    /* EN: The descriptor is architecture evidence, not a usable protocol API.
     * 中文：描述符只证明架构边界，不是可使用的协议 API。 */
    UCN_I_MODULE_SCAFFOLD_ONLY = UINT16_C(1) << 0,
    UCN_I_MODULE_FAILS_CLOSED = UINT16_C(1) << 1,
    UCN_I_MODULE_PRIVATE = UINT16_C(1) << 2
};

/* EN: A scaffold descriptor freezes physical ownership and dependency
 * direction before any business state or public ABI is introduced. A bit in
 * coordinator_visible_mask means immutable facts/requirements may be routed
 * through the Coordinator; it never authorizes a direct Owner call.
 * 中文：骨架描述符在引入业务状态或公共 ABI 前冻结物理所有权和依赖方向。
 * coordinator_visible_mask 中的位只表示可经 Coordinator 路由不可变事实或
 * requirement，绝不授权 Owner 之间直接调用。 */
typedef struct ucn_i_module_scaffold_descriptor {
    uint16_t struct_size;
    uint16_t schema;
    uint8_t module_id;
    uint8_t implementation_stage;
    uint16_t direct_dependency_mask;
    uint16_t coordinator_visible_mask;
    uint16_t forbidden_direct_owner_mask;
    uint16_t flags;
    uint16_t reserved_zero;
} ucn_i_module_scaffold_descriptor_t;

UCN_STATIC_ASSERT(sizeof(ucn_i_module_scaffold_descriptor_t) == 16U,
                  module_scaffold_descriptor_must_be_16_bytes);

ucn_result_t ucn_i_module_scaffold_validate(
    const ucn_i_module_scaffold_descriptor_t *descriptor);

/* EN: Until a module's detailed implementation task is complete, every
 * business entry fails closed and leaves its output untouched.
 * 中文：模块详细实现任务完成前，所有业务入口都必须失败关闭且不改输出。 */
ucn_result_t ucn_i_module_scaffold_business_probe(
    const ucn_i_module_scaffold_descriptor_t *descriptor,
    uint32_t *output_unchanged);

const ucn_i_module_scaffold_descriptor_t *ucn_i_security_scaffold(void);
const ucn_i_module_scaffold_descriptor_t *ucn_i_admission_scaffold(void);
const ucn_i_module_scaffold_descriptor_t *ucn_i_routing_scaffold(void);
const ucn_i_module_scaffold_descriptor_t *ucn_i_transport_scaffold(void);
const ucn_i_module_scaffold_descriptor_t *ucn_i_service_scaffold(void);
const ucn_i_module_scaffold_descriptor_t *ucn_i_realtime_scaffold(void);
const ucn_i_module_scaffold_descriptor_t *ucn_i_group_scaffold(void);
const ucn_i_module_scaffold_descriptor_t *ucn_i_cluster_scaffold(void);

#endif
