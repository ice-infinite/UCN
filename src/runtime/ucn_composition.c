#include "internal/ucn_composition.h"

#include "internal/ucn_admission.h"
#include "internal/ucn_capability.h"
#include "internal/ucn_checked.h"
#include "internal/ucn_cluster.h"
#include "internal/ucn_coordinator.h"
#include "internal/ucn_flow.h"
#include "internal/ucn_group.h"
#include "internal/ucn_identity.h"
#include "internal/ucn_persistence.h"
#include "internal/ucn_realtime.h"
#include "internal/ucn_route.h"
#include "internal/ucn_runtime.h"
#include "internal/ucn_security.h"
#include "internal/ucn_service.h"
#include "internal/ucn_transport.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define UCN_I_COMPOSITION_RUNTIME_MAGIC UINT32_C(0x55434352)
#define UCN_I_COMPOSITION_OPERATION_LIFECYCLE UINT16_C(1)

static void composition_mark_fault(
    ucn_i_composition_runtime_t *runtime);
static bool composition_control_rx_hop_state_valid(
    const ucn_i_composition_control_rx_plan_t *plan);
#define UCN_I_COMPOSITION_OP_START_BEGIN UINT32_C(1)
#define UCN_I_COMPOSITION_OP_START_STEP UINT32_C(2)
#define UCN_I_COMPOSITION_OP_STOP_BEGIN UINT32_C(3)
#define UCN_I_COMPOSITION_OP_STOP_STEP UINT32_C(4)
#define UCN_I_COMPOSITION_OP_ENDPOINT_ADD UINT32_C(10)
#define UCN_I_COMPOSITION_OP_ENDPOINT_REMOVE UINT32_C(11)
#define UCN_I_COMPOSITION_OP_PATH_ADD UINT32_C(12)
#define UCN_I_COMPOSITION_OP_PATH_REMOVE UINT32_C(13)
#define UCN_I_COMPOSITION_OP_LINK_GET UINT32_C(14)
#define UCN_I_COMPOSITION_OP_PUBLISH UINT32_C(15)
#define UCN_I_COMPOSITION_OP_SEND_QUERY UINT32_C(16)
#define UCN_I_COMPOSITION_OP_SEND_CANCEL UINT32_C(17)
#define UCN_I_COMPOSITION_OP_SEND_FORGET UINT32_C(18)
#define UCN_I_COMPOSITION_OP_STATS UINT32_C(19)
#define UCN_I_COMPOSITION_OP_RUN_STEP UINT32_C(20)
#define UCN_I_COMPOSITION_OP_DRIVER_INGRESS UINT32_C(21)
#define UCN_I_COMPOSITION_OP_ADVANCED_USE UINT32_C(22)
#define UCN_I_COMPOSITION_OP_ADVANCED_FINISH UINT32_C(23)
#define UCN_I_COMPOSITION_OP_DELIVERY_PREPARE UINT32_C(24)
#define UCN_I_COMPOSITION_OP_DELIVERY_CANCEL UINT32_C(25)
#define UCN_I_COMPOSITION_OP_DURABLE_OPERATION UINT32_C(26)
#define UCN_I_COMPOSITION_OP_REALTIME_POLICY UINT32_C(27)
#define UCN_I_COMPOSITION_OP_REALTIME_PREPARE UINT32_C(28)
#define UCN_I_COMPOSITION_OP_GROUP_INSTALL_STATIC UINT32_C(29)
#define UCN_I_COMPOSITION_OP_GROUP_SEND_BEGIN UINT32_C(30)
#define UCN_I_COMPOSITION_OP_GROUP_ATTEMPT_PEEK UINT32_C(31)
#define UCN_I_COMPOSITION_OP_CLUSTER_MEMBER_OBSERVE UINT32_C(32)
#define UCN_I_COMPOSITION_OP_CLUSTER_AUTHORITY_PREFLIGHT UINT32_C(33)
#define UCN_I_COMPOSITION_OP_CONTROL_PREPARE UINT32_C(34)
#define UCN_I_COMPOSITION_OP_CONTROL_FINISH UINT32_C(35)
#define UCN_I_COMPOSITION_OP_BOOTSTRAP_PREPARE UINT32_C(36)
#define UCN_I_COMPOSITION_OP_BOOTSTRAP_FINISH UINT32_C(37)
#define UCN_I_COMPOSITION_OP_BOOTSTRAP_TX UINT32_C(38)
#define UCN_I_COMPOSITION_OP_ROUTE_CONTROL UINT32_C(39)
#define UCN_I_COMPOSITION_OP_CAPABILITY_CONTROL UINT32_C(40)
#define UCN_I_COMPOSITION_OP_INGRESS_STEP UINT32_C(41)
#define UCN_I_COMPOSITION_OP_INGRESS_FINISH UINT32_C(42)
#define UCN_I_COMPOSITION_OP_TRANSPORT_CONTROL UINT32_C(43)
#define UCN_I_COMPOSITION_OP_SERVICE_CONTROL UINT32_C(44)

typedef struct ucn_i_composition_rule {
    ucn_i_composition_capability_mask_t capability;
    ucn_i_composition_module_mask_t required_modules;
    ucn_i_composition_module_mask_t durable_modules;
} ucn_i_composition_rule_t;

static const ucn_i_composition_rule_t composition_rules[] = {
    {UCN_I_CAP_STATIC_C1, UCN_I_COMPOSE_FOUNDATION, 0U},
    {UCN_I_CAP_PROTECTED_C1,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_SECURITY,
     UCN_I_COMPOSE_SECURITY},
    {UCN_I_CAP_DYNAMIC_ADMISSION,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
         UCN_I_COMPOSE_ADMISSION | UCN_I_COMPOSE_CAPABILITY,
     UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY},
    {UCN_I_CAP_AUTO_ROUTE,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_ROUTE, 0U},
    {UCN_I_CAP_ADVANCED_FLOW,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_CAPABILITY |
         UCN_I_COMPOSE_ROUTE | UCN_I_COMPOSE_FLOW,
     UCN_I_COMPOSE_SECURITY},
    {UCN_I_CAP_RELIABLE,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_TRANSPORT, 0U},
    {UCN_I_CAP_TRANSFER,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_TRANSPORT,
     UCN_I_COMPOSE_TRANSPORT},
    {UCN_I_CAP_SERVICE,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_SERVICE, 0U},
    {UCN_I_CAP_DURABLE_OPERATION,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_SERVICE,
     UCN_I_COMPOSE_SERVICE},
    {UCN_I_CAP_LOCAL_STAMP,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_REALTIME, 0U},
    {UCN_I_CAP_NETWORK_TIME,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_CAPABILITY |
         UCN_I_COMPOSE_ROUTE | UCN_I_COMPOSE_FLOW |
         UCN_I_COMPOSE_REALTIME,
     UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_REALTIME},
    {UCN_I_CAP_STATIC_GROUP,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_GROUP, 0U},
    {UCN_I_CAP_DYNAMIC_GROUP,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
         UCN_I_COMPOSE_GROUP,
     UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
         UCN_I_COMPOSE_GROUP},
    {UCN_I_CAP_CLUSTER,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
         UCN_I_COMPOSE_CAPABILITY | UCN_I_COMPOSE_ROUTE |
         UCN_I_COMPOSE_FLOW | UCN_I_COMPOSE_CLUSTER,
     UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
         UCN_I_COMPOSE_CLUSTER},
    {UCN_I_CAP_CLUSTER_GROUP_ACCELERATION,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
         UCN_I_COMPOSE_CAPABILITY | UCN_I_COMPOSE_ROUTE |
         UCN_I_COMPOSE_FLOW | UCN_I_COMPOSE_GROUP |
         UCN_I_COMPOSE_CLUSTER,
     UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
         UCN_I_COMPOSE_CLUSTER}
};

static const ucn_i_composition_module_mask_t module_bits[] = {
    UCN_I_COMPOSE_PERSISTENCE, UCN_I_COMPOSE_IDENTITY,
    UCN_I_COMPOSE_SECURITY,    UCN_I_COMPOSE_ADMISSION,
    UCN_I_COMPOSE_CAPABILITY,  UCN_I_COMPOSE_ROUTE,
    UCN_I_COMPOSE_FLOW,        UCN_I_COMPOSE_TRANSPORT,
    UCN_I_COMPOSE_SERVICE,     UCN_I_COMPOSE_REALTIME,
    UCN_I_COMPOSE_GROUP,       UCN_I_COMPOSE_CLUSTER
};

static bool bytes_are_zero(const void *object, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)object;
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool lock_ops_are_valid(const ucn_i_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_I_LOCK_OPS_VERSION &&
           lock->enter != NULL && lock->leave != NULL;
}

static bool pointer_is_aligned(const void *pointer, size_t alignment)
{
    return pointer != NULL && alignment != 0U &&
           ((uintptr_t)pointer % alignment) == 0U;
}

static ucn_result_t size_align_up(size_t value,
                                  size_t alignment,
                                  size_t *aligned_out)
{
    size_t remainder;
    size_t padding;

    if (alignment == 0U || aligned_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    remainder = value % alignment;
    padding = remainder == 0U ? 0U : alignment - remainder;
    return ucn_i_size_add(value, padding, aligned_out);
}

static ucn_i_composition_module_mask_t module_bit_from_id(
    ucn_i_composition_module_id_t module_id)
{
    if (module_id <= UCN_I_COMPOSITION_MODULE_CLUSTER) {
        return UINT32_C(1) << module_id;
    }
    return 0U;
}

static ucn_i_composition_module_id_t dependency_target_module(
    ucn_i_dependency_kind_t dependency_kind)
{
    static const uint8_t targets[UCN_I_DEPENDENCY_KIND_COUNT] = {
        UCN_I_COMPOSITION_MODULE_IDENTITY,
        UCN_I_COMPOSITION_MODULE_SECURITY,
        UCN_I_COMPOSITION_MODULE_CAPABILITY,
        UCN_I_COMPOSITION_MODULE_ROUTE,
        UCN_I_COMPOSITION_MODULE_FLOW,
        UCN_I_COMPOSITION_MODULE_TRANSPORT,
        UCN_I_COMPOSITION_MODULE_GROUP,
        UCN_I_COMPOSITION_MODULE_REALTIME,
        UCN_I_COMPOSITION_MODULE_PERSISTENCE
    };

    if (dependency_kind < UCN_I_DEP_IDENTITY_BINDING ||
        dependency_kind > UCN_I_DEP_PERSISTENCE) {
        return UINT8_MAX;
    }
    return targets[dependency_kind - 1U];
}

static bool owner_ref_equal(const ucn_i_composition_owner_ref_t *left,
                            const ucn_i_composition_owner_ref_t *right)
{
    return left != NULL && right != NULL &&
           memcmp(left, right, sizeof(*left)) == 0;
}

static const ucn_i_composition_owner_record_t *owner_record_find(
    const ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id)
{
    uint8_t index;

    for (index = 0U; index < runtime->registry_count; ++index) {
        if (runtime->registry[index].ref.module_id == module_id) {
            return &runtime->registry[index];
        }
    }
    return NULL;
}

static ucn_i_composition_owner_record_t *owner_record_find_mutable(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id)
{
    uint8_t index;

    for (index = 0U; index < runtime->registry_count; ++index) {
        if (runtime->registry[index].ref.module_id == module_id) {
            return &runtime->registry[index];
        }
    }
    return NULL;
}

static ucn_result_t composition_event_sink(
    void *context,
    uint16_t requester_owner_instance,
    const ucn_i_dependency_requirement_t *requirement,
    const ucn_i_dependency_event_t *event)
{
    (void)context;
    (void)requester_owner_instance;
    (void)requirement;
    (void)event;
    /* IMPL-09-01 deliberately has no active business Owner.  IMPL-09-02
     * replaces this fail-closed sink with typed Owner event dispatch only
     * after durable reload and Owner publication complete. */
    return UCN_ERR_STATE;
}

static bool profile_is_valid(uint32_t profile)
{
    return profile == UCN_PROFILE_NANO || profile == UCN_PROFILE_LITE ||
           profile == UCN_PROFILE_FULL;
}

static ucn_i_composition_module_mask_t intrinsic_requirements(
    ucn_i_composition_module_mask_t modules)
{
    ucn_i_composition_module_mask_t required = UCN_I_COMPOSE_FOUNDATION;

    if ((modules & UCN_I_COMPOSE_IDENTITY) != 0U) {
        required |= UCN_I_COMPOSE_PERSISTENCE;
    }
    if ((modules & UCN_I_COMPOSE_SECURITY) != 0U) {
        required |= UCN_I_COMPOSE_PERSISTENCE;
    }
    if ((modules & UCN_I_COMPOSE_ADMISSION) != 0U) {
        required |= UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_IDENTITY |
                    UCN_I_COMPOSE_SECURITY;
    }
    if ((modules & UCN_I_COMPOSE_CAPABILITY) != 0U) {
        required |= UCN_I_COMPOSE_SECURITY;
    }
    if ((modules & UCN_I_COMPOSE_FLOW) != 0U) {
        required |= UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_SECURITY |
                    UCN_I_COMPOSE_CAPABILITY | UCN_I_COMPOSE_ROUTE;
    }
    if ((modules & UCN_I_COMPOSE_CLUSTER) != 0U) {
        required |= UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_IDENTITY |
                    UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_CAPABILITY |
                    UCN_I_COMPOSE_ROUTE | UCN_I_COMPOSE_FLOW;
    }
    return required;
}

static size_t module_owner_bytes(ucn_i_composition_module_mask_t module)
{
    switch (module) {
        case UCN_I_COMPOSE_PERSISTENCE:
            return sizeof(ucn_persistence_storage_t);
        case UCN_I_COMPOSE_IDENTITY:
            return sizeof(ucn_i_identity_owner_t);
        case UCN_I_COMPOSE_SECURITY:
            return sizeof(ucn_i_security_owner_t);
        case UCN_I_COMPOSE_ADMISSION:
            return sizeof(ucn_i_admission_owner_t);
        case UCN_I_COMPOSE_CAPABILITY:
            return sizeof(ucn_i_capability_owner_t);
        case UCN_I_COMPOSE_ROUTE:
            return sizeof(ucn_i_route_owner_t);
        case UCN_I_COMPOSE_FLOW:
            return sizeof(ucn_i_flow_owner_t);
        case UCN_I_COMPOSE_TRANSPORT:
            return sizeof(ucn_i_transport_owner_t);
        case UCN_I_COMPOSE_SERVICE:
            return sizeof(ucn_i_service_owner_t);
        case UCN_I_COMPOSE_REALTIME:
            return sizeof(ucn_i_realtime_owner_t);
        case UCN_I_COMPOSE_GROUP:
            return sizeof(ucn_i_group_owner_t);
        case UCN_I_COMPOSE_CLUSTER:
            return sizeof(ucn_i_cluster_owner_t);
        default:
            return 0U;
    }
}

static uint64_t digest_u32(uint64_t digest, uint32_t value)
{
    uint8_t index;

    for (index = 0U; index < 4U; ++index) {
        const uint8_t shift = (uint8_t)(24U - (index * 8U));
        digest ^= (uint64_t)((value >> shift) & UINT32_C(0xFF));
        digest *= UINT64_C(0x00000100000001B3);
    }
    return digest;
}

static uint64_t composition_digest(const ucn_i_composition_plan_t *plan)
{
    uint64_t digest = UINT64_C(0xCBF29CE484222325);
    uint8_t index;

    digest = digest_u32(digest, UINT32_C(0x55434E43));
    digest = digest_u32(digest, UCN_I_COMPOSITION_SCHEMA);
    digest = digest_u32(digest, plan->profile);
    digest = digest_u32(digest, plan->available_module_mask);
    digest = digest_u32(digest, plan->compiled_module_mask);
    digest = digest_u32(digest, plan->selected_module_mask);
    digest = digest_u32(digest, plan->durable_module_mask);
    digest = digest_u32(digest, plan->capability_mask);
    digest = digest_u32(digest, plan->private_owner_bytes);
    digest = digest_u32(digest, plan->init_count);
    for (index = 0U; index < plan->init_count; ++index) {
        digest = digest_u32(digest, plan->init_order[index]);
    }
    return digest;
}

static bool request_is_well_formed(
    const ucn_i_composition_request_t *request)
{
    return request != NULL && request->struct_size == sizeof(*request) &&
           request->schema == UCN_I_COMPOSITION_SCHEMA &&
           profile_is_valid(request->profile) && request->reserved_zero == 0U &&
           (request->available_module_mask & ~UCN_I_COMPOSE_KNOWN_MODULES) ==
               0U &&
           (request->available_module_mask & UCN_I_COMPOSE_FOUNDATION) != 0U &&
           (request->capability_mask & ~UCN_I_COMPOSE_KNOWN_CAPABILITIES) ==
               0U &&
           (request->capability_mask & UCN_I_CAP_STATIC_C1) != 0U;
}

ucn_result_t ucn_i_composition_plan_build(
    const ucn_i_composition_request_t *request,
    ucn_i_composition_plan_t *plan_out)
{
    ucn_i_composition_plan_t plan;
    ucn_i_composition_module_mask_t selected = UCN_I_COMPOSE_FOUNDATION;
    ucn_i_composition_module_mask_t durable = 0U;
    size_t private_owner_bytes = sizeof(ucn_i_coordinator_t);
    size_t index;

    if (!request_is_well_formed(request) || plan_out == NULL ||
        ucn_i_ranges_overlap(request, sizeof(*request), plan_out,
                             sizeof(*plan_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if ((request->available_module_mask &
         ~UCN_I_COMPOSITION_COMPILED_MODULE_MASK) != 0U) {
        return UCN_ERR_UNSUPPORTED;
    }

    for (index = 0U;
         index < sizeof(composition_rules) / sizeof(composition_rules[0]);
         ++index) {
        if ((request->capability_mask & composition_rules[index].capability) !=
            0U) {
            selected |= composition_rules[index].required_modules;
            durable |= composition_rules[index].durable_modules;
        }
    }
    selected |= intrinsic_requirements(selected);
    if ((selected & ~request->available_module_mask) != 0U) {
        return UCN_ERR_CONFIG;
    }

    memset(&plan, 0, sizeof(plan));
    plan.struct_size = (uint16_t)sizeof(plan);
    plan.schema = UCN_I_COMPOSITION_SCHEMA;
    plan.profile = request->profile;
    plan.available_module_mask = request->available_module_mask;
    plan.compiled_module_mask = UCN_I_COMPOSITION_COMPILED_MODULE_MASK;
    plan.selected_module_mask = selected;
    plan.durable_module_mask = durable;
    plan.capability_mask = request->capability_mask;

    for (index = 0U; index < sizeof(module_bits) / sizeof(module_bits[0]);
         ++index) {
        if ((selected & module_bits[index]) != 0U) {
            const size_t owner_bytes = module_owner_bytes(module_bits[index]);
            if (owner_bytes == 0U ||
                private_owner_bytes > UINT32_MAX - owner_bytes ||
                plan.init_count >= UCN_I_COMPOSITION_INIT_ORDER_CAPACITY) {
                return UCN_ERR_NO_SPACE;
            }
            private_owner_bytes += owner_bytes;
            plan.init_order[plan.init_count] = (uint8_t)(index + 1U);
            ++plan.init_count;
        }
    }
    plan.private_owner_bytes = (uint32_t)private_owner_bytes;
    plan.composition_digest = composition_digest(&plan);
    if (plan.composition_digest == 0U) {
        return UCN_ERR_STATE;
    }
    *plan_out = plan;
    return UCN_OK;
}

bool ucn_i_composition_plan_valid(const ucn_i_composition_plan_t *plan)
{
    ucn_i_composition_module_mask_t selected = UCN_I_COMPOSE_FOUNDATION;
    ucn_i_composition_module_mask_t durable = 0U;
    size_t private_owner_bytes = sizeof(ucn_i_coordinator_t);
    size_t index;
    uint8_t expected_init_count = 0U;

    if (plan == NULL || plan->struct_size != sizeof(*plan) ||
        plan->schema != UCN_I_COMPOSITION_SCHEMA ||
        !profile_is_valid(plan->profile) ||
        plan->reserved_zero[0] != 0U || plan->reserved_zero[1] != 0U ||
        plan->reserved_zero[2] != 0U ||
        (plan->available_module_mask & ~UCN_I_COMPOSE_KNOWN_MODULES) != 0U ||
        (plan->available_module_mask &
         ~UCN_I_COMPOSITION_COMPILED_MODULE_MASK) != 0U ||
        plan->compiled_module_mask != UCN_I_COMPOSITION_COMPILED_MODULE_MASK ||
        (plan->capability_mask & ~UCN_I_COMPOSE_KNOWN_CAPABILITIES) != 0U ||
        (plan->capability_mask & UCN_I_CAP_STATIC_C1) == 0U) {
        return false;
    }

    if ((plan->available_module_mask & UCN_I_COMPOSE_FOUNDATION) == 0U) {
        return false;
    }

    for (index = 0U;
         index < sizeof(composition_rules) / sizeof(composition_rules[0]);
         ++index) {
        if ((plan->capability_mask & composition_rules[index].capability) !=
            0U) {
            selected |= composition_rules[index].required_modules;
            durable |= composition_rules[index].durable_modules;
        }
    }
    selected |= intrinsic_requirements(selected);
    if ((selected & ~plan->available_module_mask) != 0U ||
        plan->selected_module_mask != selected ||
        plan->durable_module_mask != durable) {
        return false;
    }

    for (index = 0U; index < sizeof(module_bits) / sizeof(module_bits[0]);
         ++index) {
        if ((selected & module_bits[index]) != 0U) {
            const size_t owner_bytes = module_owner_bytes(module_bits[index]);

            if (owner_bytes == 0U ||
                private_owner_bytes > UINT32_MAX - owner_bytes ||
                expected_init_count >= UCN_I_COMPOSITION_INIT_ORDER_CAPACITY ||
                plan->init_order[expected_init_count] !=
                    (uint8_t)(index + 1U)) {
                return false;
            }
            private_owner_bytes += owner_bytes;
            ++expected_init_count;
        }
    }
    if (plan->init_count != expected_init_count ||
        plan->private_owner_bytes != (uint32_t)private_owner_bytes) {
        return false;
    }
    for (index = expected_init_count;
         index < UCN_I_COMPOSITION_INIT_ORDER_CAPACITY; ++index) {
        if (plan->init_order[index] != 0U) {
            return false;
        }
    }
    return plan->composition_digest != 0U &&
           plan->composition_digest == composition_digest(plan);
}

static ucn_result_t composition_layout_required(
    const ucn_i_composition_plan_t *plan,
    size_t *storage_bytes_out)
{
    size_t offset;
    uint8_t index;

    if (!ucn_i_composition_plan_valid(plan) || storage_bytes_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (size_align_up(sizeof(ucn_i_composition_runtime_t),
                      UCN_I_COMPOSITION_STORAGE_ALIGNMENT,
                      &offset) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    for (index = 0U; index < plan->init_count; ++index) {
        const ucn_i_composition_module_mask_t module =
            module_bit_from_id(plan->init_order[index]);
        const size_t owner_bytes = module_owner_bytes(module);

        if (module == 0U || owner_bytes == 0U ||
            size_align_up(offset, UCN_I_COMPOSITION_STORAGE_ALIGNMENT,
                          &offset) != UCN_OK ||
            ucn_i_size_add(offset, owner_bytes, &offset) != UCN_OK) {
            return UCN_ERR_NO_SPACE;
        }
    }
    if (offset > UINT32_MAX) {
        return UCN_ERR_NO_SPACE;
    }
    *storage_bytes_out = offset;
    return UCN_OK;
}

ucn_result_t ucn_i_composition_storage_required(
    const ucn_i_composition_plan_t *plan,
    size_t *storage_bytes_out)
{
    size_t required;
    ucn_result_t result;

    if (storage_bytes_out == NULL ||
        (plan != NULL &&
         ucn_i_ranges_overlap(plan, sizeof(*plan), storage_bytes_out,
                              sizeof(*storage_bytes_out)))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_layout_required(plan, &required);
    if (result != UCN_OK) {
        return result;
    }
    *storage_bytes_out = required;
    return UCN_OK;
}

static void owner_record_fill(ucn_i_composition_owner_record_t *record,
                              uint32_t runtime_instance,
                              uint16_t owner_instance_base,
                              ucn_i_composition_module_id_t module_id,
                              ucn_i_composition_module_mask_t module_mask,
                              uint32_t storage_offset,
                              uint32_t storage_bytes,
                              ucn_i_composition_owner_state_t state)
{
    memset(record, 0, sizeof(*record));
    record->ref.runtime_instance = runtime_instance;
    record->ref.owner_instance =
        (uint16_t)(owner_instance_base + (uint16_t)module_id);
    record->ref.generation = UINT16_C(1);
    record->ref.module_id = module_id;
    record->module_mask = module_mask;
    record->storage_offset = storage_offset;
    record->storage_bytes = storage_bytes;
    record->state = state;
}

static bool prepare_config_is_valid(
    const ucn_i_composition_prepare_config_t *config)
{
    return config != NULL && config->struct_size == sizeof(*config) &&
           config->schema == UCN_I_COMPOSITION_RUNTIME_SCHEMA &&
           config->runtime_instance != 0U &&
           config->owner_instance_base > UCN_I_ADAPTER_OWNER_INSTANCE &&
           config->owner_instance_base <=
               UINT16_MAX - UCN_I_COMPOSITION_MODULE_COORDINATOR &&
           config->reserved_zero == 0U &&
           lock_ops_are_valid(&config->coordinator_lock);
}

ucn_result_t ucn_i_composition_prepare(
    void *storage,
    size_t storage_bytes,
    const ucn_i_composition_plan_t *plan,
    const ucn_i_composition_prepare_config_t *config,
    ucn_i_composition_runtime_t **runtime_out)
{
    ucn_i_composition_runtime_t *runtime;
    size_t required;
    size_t offset;
    uint8_t index;
    ucn_result_t result;

    if (runtime_out == NULL ||
        composition_layout_required(plan, &required) != UCN_OK ||
        !prepare_config_is_valid(config) || storage == NULL ||
        !pointer_is_aligned(storage, UCN_I_COMPOSITION_STORAGE_ALIGNMENT) ||
        storage_bytes != required || required > UINT32_MAX ||
        ucn_i_ranges_overlap(storage, required, plan, sizeof(*plan)) ||
        ucn_i_ranges_overlap(storage, required, config, sizeof(*config)) ||
        ucn_i_ranges_overlap(storage, required, runtime_out,
                             sizeof(*runtime_out)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), runtime_out,
                             sizeof(*runtime_out)) ||
        ucn_i_ranges_overlap(config, sizeof(*config), runtime_out,
                             sizeof(*runtime_out)) ||
        ucn_i_ranges_overlap(config->coordinator_lock.context, 1U, storage,
                             required) ||
        !bytes_are_zero(storage, required)) {
        return UCN_ERR_ARGUMENT;
    }

    runtime = (ucn_i_composition_runtime_t *)storage;
    runtime->runtime_instance = config->runtime_instance;
    runtime->storage_bytes = (uint32_t)required;
    runtime->schema = UCN_I_COMPOSITION_RUNTIME_SCHEMA;
    runtime->owner_instance_base = config->owner_instance_base;
    runtime->phase = UCN_I_COMPOSITION_PREPARED;
    runtime->plan = *plan;

    owner_record_fill(&runtime->registry[runtime->registry_count++],
                      config->runtime_instance,
                      config->owner_instance_base,
                      UCN_I_COMPOSITION_MODULE_FOUNDATION,
                      UCN_I_COMPOSE_FOUNDATION, 0U, 0U,
                      UCN_I_COMPOSITION_OWNER_REGISTERED);
    owner_record_fill(&runtime->registry[runtime->registry_count++],
                      config->runtime_instance,
                      config->owner_instance_base,
                      UCN_I_COMPOSITION_MODULE_COORDINATOR, 0U,
                      (uint32_t)offsetof(ucn_i_composition_runtime_t,
                                         coordinator),
                      (uint32_t)sizeof(runtime->coordinator),
                      UCN_I_COMPOSITION_OWNER_ACTIVE);

    result = size_align_up(sizeof(*runtime),
                           UCN_I_COMPOSITION_STORAGE_ALIGNMENT, &offset);
    if (result != UCN_OK) {
        memset(storage, 0, required);
        return UCN_ERR_NO_SPACE;
    }
    for (index = 0U; index < plan->init_count; ++index) {
        const ucn_i_composition_module_id_t module_id =
            plan->init_order[index];
        const ucn_i_composition_module_mask_t module =
            module_bit_from_id(module_id);
        const size_t owner_bytes = module_owner_bytes(module);

        result = size_align_up(offset, UCN_I_COMPOSITION_STORAGE_ALIGNMENT,
                               &offset);
        if (result != UCN_OK || offset > UINT32_MAX ||
            owner_bytes > UINT32_MAX) {
            memset(storage, 0, required);
            return UCN_ERR_NO_SPACE;
        }
        owner_record_fill(&runtime->registry[runtime->registry_count++],
                          config->runtime_instance,
                          config->owner_instance_base, module_id, module,
                          (uint32_t)offset, (uint32_t)owner_bytes,
                          UCN_I_COMPOSITION_OWNER_RESERVED);
        offset += owner_bytes;
    }

    result = ucn_i_coordinator_init(
        &runtime->coordinator, config->runtime_instance,
        (uint16_t)(config->owner_instance_base +
                   UCN_I_COMPOSITION_MODULE_COORDINATOR),
        &config->coordinator_lock, ucn_i_requirement_digest_default,
        composition_event_sink, runtime);
    if (result != UCN_OK) {
        memset(storage, 0, required);
        return result;
    }
    result = ucn_i_callback_gate_init(
        &runtime->lifecycle_gate,
        config->runtime_instance,
        &config->coordinator_lock);
    if (result != UCN_OK) {
        (void)ucn_i_coordinator_destroy(&runtime->coordinator);
        memset(storage, 0, required);
        return result;
    }
    runtime->magic = UCN_I_COMPOSITION_RUNTIME_MAGIC;
    *runtime_out = runtime;
    return UCN_OK;
}

static bool owner_record_is_exact(
    const ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_owner_record_t *record,
    ucn_i_composition_module_id_t module_id,
    ucn_i_composition_module_mask_t module_mask,
    uint32_t storage_offset,
    uint32_t storage_bytes,
    ucn_i_composition_owner_state_t state)
{
    return record->ref.runtime_instance == runtime->runtime_instance &&
           record->ref.owner_instance ==
               (uint16_t)(runtime->owner_instance_base + module_id) &&
           record->ref.generation == UINT16_C(1) &&
           record->ref.module_id == module_id &&
           bytes_are_zero(record->ref.reserved_zero,
                          sizeof(record->ref.reserved_zero)) &&
           record->module_mask == module_mask &&
           record->storage_offset == storage_offset &&
           record->storage_bytes == storage_bytes && record->state == state &&
           bytes_are_zero(record->reserved_zero,
                          sizeof(record->reserved_zero));
}

static bool coordinator_is_pristine(
    const ucn_i_composition_runtime_t *runtime)
{
    const ucn_i_coordinator_t *coordinator = &runtime->coordinator;

    return coordinator->magic == UCN_I_COORDINATOR_MAGIC &&
           coordinator->runtime_instance == runtime->runtime_instance &&
           coordinator->owner_instance ==
               (uint16_t)(runtime->owner_instance_base +
                          UCN_I_COMPOSITION_MODULE_COORDINATOR) &&
           coordinator->schema == UCN_I_COORDINATOR_SCHEMA &&
           coordinator->route_active == 0U && coordinator->faulted == 0U &&
           coordinator->reserved_zero == 0U &&
           coordinator->event_sink_context == runtime &&
           coordinator->digest == ucn_i_requirement_digest_default &&
           coordinator->event_sink == composition_event_sink &&
           lock_ops_are_valid(&coordinator->lock) &&
           !ucn_i_ranges_overlap(coordinator->lock.context, 1U, runtime,
                                 runtime->storage_bytes) &&
           bytes_are_zero(coordinator->bindings,
                          sizeof(coordinator->bindings)) &&
           bytes_are_zero(coordinator->slots, sizeof(coordinator->slots));
}

static bool lifecycle_gate_is_idle(
    const ucn_i_composition_runtime_t *runtime)
{
    const ucn_i_callback_gate_t *gate = &runtime->lifecycle_gate;

    return gate->magic == UCN_I_CALLBACK_GATE_MAGIC &&
           gate->gate_instance == runtime->runtime_instance &&
           gate->schema == UCN_I_CALLBACK_GATE_SCHEMA &&
           gate->reserved_zero == 0U &&
           bytes_are_zero(&gate->active_claim, sizeof(gate->active_claim)) &&
           lock_ops_are_valid(&gate->lock) &&
           !ucn_i_ranges_overlap(gate->lock.context, 1U, runtime,
                                 runtime->storage_bytes);
}

static bool composition_token_is_zero(ucn_driver_token_t token)
{
    static const ucn_driver_token_t zero_token = {0};

    return memcmp(&token, &zero_token, sizeof(token)) == 0;
}

static bool composition_token_equal(ucn_driver_token_t left,
                                    ucn_driver_token_t right)
{
    return memcmp(&left, &right, sizeof(left)) == 0;
}

static bool composition_ingress_state_valid(
    const ucn_i_composition_runtime_t *runtime)
{
    const bool token_zero =
        composition_token_is_zero(runtime->ingress_pending_token);

    if (runtime->ingress_cursor >= UCN_I_COMPOSITION_INGRESS_CLASS_COUNT ||
        runtime->ingress_pending_kind >
            UCN_I_COMPOSITION_INGRESS_CONTROL_RELAY) {
        return false;
    }
    if (runtime->ingress_pending_kind == UCN_I_COMPOSITION_INGRESS_NONE) {
        return token_zero;
    }
    return !token_zero && runtime->ingress_generation != 0U &&
           runtime->ingress_pending_token.runtime_instance ==
               runtime->runtime_instance &&
           runtime->ingress_pending_token.object_kind ==
               UCN_OBJECT_KIND_ADAPTER_RX &&
           runtime->ingress_pending_token.reserved_zero == 0U;
}

static ucn_i_composition_owner_state_t expected_foundation_state(
    ucn_i_composition_runtime_phase_t phase)
{
    if (phase == UCN_I_COMPOSITION_RUNNING) {
        return UCN_I_COMPOSITION_OWNER_ACTIVE;
    }
    if (phase == UCN_I_COMPOSITION_RELOADING ||
        phase == UCN_I_COMPOSITION_OWNERS_PUBLISHED ||
        phase == UCN_I_COMPOSITION_STOPPING ||
        phase == UCN_I_COMPOSITION_FAULT) {
        return UCN_I_COMPOSITION_OWNER_INITIALIZED;
    }
    return UCN_I_COMPOSITION_OWNER_REGISTERED;
}

static ucn_i_composition_owner_state_t expected_business_state(
    const ucn_i_composition_runtime_t *runtime,
    uint8_t plan_index)
{
    if (plan_index >= runtime->initialized_count) {
        return UCN_I_COMPOSITION_OWNER_RESERVED;
    }
    if (runtime->phase == UCN_I_COMPOSITION_OWNERS_PUBLISHED ||
        runtime->phase == UCN_I_COMPOSITION_RUNNING) {
        return UCN_I_COMPOSITION_OWNER_ACTIVE;
    }
    return UCN_I_COMPOSITION_OWNER_INITIALIZED;
}

bool ucn_i_composition_runtime_valid(
    const ucn_i_composition_runtime_t *runtime)
{
    size_t required;
    size_t offset;
    uint8_t record_index;
    uint8_t plan_index;

    if (runtime == NULL ||
        !pointer_is_aligned(runtime, UCN_I_COMPOSITION_STORAGE_ALIGNMENT) ||
        runtime->magic != UCN_I_COMPOSITION_RUNTIME_MAGIC ||
        runtime->runtime_instance == 0U ||
        runtime->schema != UCN_I_COMPOSITION_RUNTIME_SCHEMA ||
        runtime->owner_instance_base <= UCN_I_ADAPTER_OWNER_INSTANCE ||
        runtime->owner_instance_base >
            UINT16_MAX - UCN_I_COMPOSITION_MODULE_COORDINATOR ||
        (runtime->phase < UCN_I_COMPOSITION_PREPARED ||
         runtime->phase > UCN_I_COMPOSITION_FAULT) ||
        runtime->registry_count != (uint8_t)(runtime->plan.init_count + 2U) ||
        runtime->registry_count > UCN_I_COMPOSITION_REGISTRY_CAPACITY ||
        runtime->initialized_count > runtime->plan.init_count ||
        !bytes_are_zero(runtime->reserved_zero,
                        sizeof(runtime->reserved_zero)) ||
#if UCN_FEATURE_PERSISTENCE_ENABLED
        !bytes_are_zero(&runtime->lifecycle_step,
                        sizeof(runtime->lifecycle_step)) ||
#endif
        composition_layout_required(&runtime->plan, &required) != UCN_OK ||
        runtime->storage_bytes != required) {
        return false;
    }
    if (!owner_record_is_exact(
            runtime, &runtime->registry[0],
            UCN_I_COMPOSITION_MODULE_FOUNDATION, UCN_I_COMPOSE_FOUNDATION,
            0U, 0U, expected_foundation_state(runtime->phase)) ||
        !owner_record_is_exact(
            runtime, &runtime->registry[1],
            UCN_I_COMPOSITION_MODULE_COORDINATOR, 0U,
            (uint32_t)offsetof(ucn_i_composition_runtime_t, coordinator),
            (uint32_t)sizeof(runtime->coordinator),
            UCN_I_COMPOSITION_OWNER_ACTIVE) ||
        !coordinator_is_pristine(runtime) || !lifecycle_gate_is_idle(runtime) ||
        size_align_up(sizeof(*runtime), UCN_I_COMPOSITION_STORAGE_ALIGNMENT,
                      &offset) != UCN_OK) {
        return false;
    }
    if ((runtime->phase == UCN_I_COMPOSITION_PREPARED ||
         runtime->phase == UCN_I_COMPOSITION_QUIESCENT) !=
            (runtime->foundation_node == NULL) ||
        (runtime->foundation_node == NULL) !=
            (runtime->foundation_storage_bytes == 0U) ||
        (runtime->foundation_node != NULL &&
         (runtime->foundation_storage_bytes != ucn_storage_required() ||
          runtime->foundation_node->runtime_instance !=
              runtime->registry[0].ref.runtime_instance ||
          runtime->foundation_node->owner_instance !=
              runtime->registry[0].ref.owner_instance ||
          ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                               runtime->foundation_node,
                               runtime->foundation_storage_bytes))) ||
        ((runtime->phase == UCN_I_COMPOSITION_PREPARED ||
          runtime->phase == UCN_I_COMPOSITION_QUIESCENT) &&
         (runtime->initialized_count != 0U ||
          runtime->persistence_ready != 0U)) ||
        (runtime->phase == UCN_I_COMPOSITION_RELOADING &&
         (runtime->initialized_count != runtime->plan.init_count ||
          runtime->persistence_ready != 0U)) ||
        ((runtime->phase == UCN_I_COMPOSITION_OWNERS_PUBLISHED ||
          runtime->phase == UCN_I_COMPOSITION_RUNNING) &&
         runtime->initialized_count != runtime->plan.init_count) ||
        runtime->persistence_ready > 1U ||
        !composition_ingress_state_valid(runtime)) {
        return false;
    }
    record_index = 2U;
    for (plan_index = 0U; plan_index < runtime->plan.init_count;
         ++plan_index, ++record_index) {
        const ucn_i_composition_module_id_t module_id =
            runtime->plan.init_order[plan_index];
        const ucn_i_composition_module_mask_t module =
            module_bit_from_id(module_id);
        const size_t owner_bytes = module_owner_bytes(module);

        if (size_align_up(offset, UCN_I_COMPOSITION_STORAGE_ALIGNMENT,
                          &offset) != UCN_OK ||
            offset > UINT32_MAX || owner_bytes > UINT32_MAX ||
            !owner_record_is_exact(
                runtime, &runtime->registry[record_index], module_id, module,
                (uint32_t)offset, (uint32_t)owner_bytes,
                expected_business_state(runtime, plan_index)) ||
            (plan_index >= runtime->initialized_count &&
             !bytes_are_zero((const uint8_t *)runtime + offset,
                             owner_bytes)) ||
            (plan_index < runtime->initialized_count &&
             bytes_are_zero((const uint8_t *)runtime + offset,
                            owner_bytes))) {
            return false;
        }
        offset += owner_bytes;
    }
    for (; record_index < UCN_I_COMPOSITION_REGISTRY_CAPACITY;
         ++record_index) {
        if (!bytes_are_zero(&runtime->registry[record_index],
                            sizeof(runtime->registry[record_index]))) {
            return false;
        }
    }
    return offset == required;
}

ucn_result_t ucn_i_composition_owner_view(
    const ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id,
    ucn_i_composition_owner_view_t *view_out)
{
    const ucn_i_composition_owner_record_t *record;
    ucn_i_composition_owner_view_t view;

    if (!ucn_i_composition_runtime_valid(runtime) || view_out == NULL ||
        module_id > UCN_I_COMPOSITION_MODULE_COORDINATOR ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, view_out,
                             sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    record = owner_record_find(runtime, module_id);
    if (record == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    memset(&view, 0, sizeof(view));
    view.ref = record->ref;
    view.module_mask = record->module_mask;
    view.storage_offset = record->storage_offset;
    view.storage_bytes = record->storage_bytes;
    view.state = record->state;
    *view_out = view;
    return UCN_OK;
}

static bool typed_adapter_is_exact(
    const ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_typed_adapter_t *adapter)
{
    const ucn_i_composition_owner_record_t *requester;
    const ucn_i_composition_owner_record_t *target;
    const ucn_i_composition_module_id_t target_module =
        adapter == NULL ? UINT8_MAX :
                          dependency_target_module(adapter->dependency_kind);

    if (adapter == NULL || adapter->struct_size != sizeof(*adapter) ||
        adapter->schema != UCN_I_COMPOSITION_ADAPTER_SCHEMA ||
        adapter->composition_digest != runtime->plan.composition_digest ||
        target_module == UINT8_MAX ||
        !bytes_are_zero(adapter->reserved_zero,
                        sizeof(adapter->reserved_zero))) {
        return false;
    }
    requester = owner_record_find(runtime, adapter->requester.module_id);
    target = owner_record_find(runtime, target_module);
    return requester != NULL && target != NULL &&
           owner_ref_equal(&requester->ref, &adapter->requester) &&
           owner_ref_equal(&target->ref, &adapter->target);
}

ucn_result_t ucn_i_composition_typed_adapter_build(
    const ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_owner_ref_t *requester,
    ucn_i_dependency_kind_t dependency_kind,
    ucn_i_composition_typed_adapter_t *adapter_out)
{
    const ucn_i_composition_owner_record_t *requester_record;
    const ucn_i_composition_owner_record_t *target_record;
    const ucn_i_composition_module_id_t target_module =
        dependency_target_module(dependency_kind);
    ucn_i_composition_typed_adapter_t adapter;

    if (!ucn_i_composition_runtime_valid(runtime) || requester == NULL ||
        adapter_out == NULL || target_module == UINT8_MAX ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, requester,
                             sizeof(*requester)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, adapter_out,
                             sizeof(*adapter_out)) ||
        ucn_i_ranges_overlap(requester, sizeof(*requester), adapter_out,
                             sizeof(*adapter_out))) {
        return UCN_ERR_ARGUMENT;
    }
    requester_record = owner_record_find(runtime, requester->module_id);
    if (requester_record == NULL ||
        !owner_ref_equal(&requester_record->ref, requester)) {
        return UCN_ERR_STATE;
    }
    target_record = owner_record_find(runtime, target_module);
    if (target_record == NULL) {
        return UCN_ERR_UNSUPPORTED;
    }
    memset(&adapter, 0, sizeof(adapter));
    adapter.struct_size = (uint16_t)sizeof(adapter);
    adapter.schema = UCN_I_COMPOSITION_ADAPTER_SCHEMA;
    adapter.composition_digest = runtime->plan.composition_digest;
    adapter.requester = *requester;
    adapter.target = target_record->ref;
    adapter.dependency_kind = dependency_kind;
    *adapter_out = adapter;
    return UCN_OK;
}

ucn_result_t ucn_i_composition_route_requirement(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_typed_adapter_t *adapter,
    const ucn_i_dependency_requirement_t *requirement,
    uint64_t now_us,
    ucn_handle_t *handle_out)
{
    const ucn_i_composition_owner_record_t *requester;
    const ucn_i_composition_owner_record_t *target;

    if (!ucn_i_composition_runtime_valid(runtime) || adapter == NULL ||
        requirement == NULL || handle_out == NULL ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, adapter,
                             sizeof(*adapter)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, requirement,
                             sizeof(*requirement)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), requirement,
                             sizeof(*requirement)) ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(requirement, sizeof(*requirement), handle_out,
                             sizeof(*handle_out)) ||
        !typed_adapter_is_exact(runtime, adapter)) {
        return UCN_ERR_ARGUMENT;
    }
    requester = owner_record_find(runtime, adapter->requester.module_id);
    target = owner_record_find(runtime, adapter->target.module_id);
    if (requester == NULL || target == NULL ||
        requester->state != UCN_I_COMPOSITION_OWNER_ACTIVE ||
        target->state != UCN_I_COMPOSITION_OWNER_ACTIVE ||
        requirement->runtime_instance != runtime->runtime_instance ||
        requirement->requester_owner_instance !=
            adapter->requester.owner_instance ||
        requirement->kind != adapter->dependency_kind) {
        return UCN_ERR_STATE;
    }
    return ucn_i_coordinator_route_requirement(
        &runtime->coordinator, requirement, now_us, handle_out);
}

ucn_result_t ucn_i_composition_route_event(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_typed_adapter_t *adapter,
    const ucn_i_dependency_event_t *event,
    uint64_t now_us)
{
    const ucn_i_composition_owner_record_t *requester;
    const ucn_i_composition_owner_record_t *target;

    if (!ucn_i_composition_runtime_valid(runtime) || adapter == NULL ||
        event == NULL ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, adapter,
                             sizeof(*adapter)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, event,
                             sizeof(*event)) ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), event,
                             sizeof(*event)) ||
        !typed_adapter_is_exact(runtime, adapter)) {
        return UCN_ERR_ARGUMENT;
    }
    requester = owner_record_find(runtime, adapter->requester.module_id);
    target = owner_record_find(runtime, adapter->target.module_id);
    if (requester == NULL || target == NULL ||
        requester->state != UCN_I_COMPOSITION_OWNER_ACTIVE ||
        target->state != UCN_I_COMPOSITION_OWNER_ACTIVE ||
        event->runtime_instance != runtime->runtime_instance ||
        event->owner_instance != adapter->target.owner_instance ||
        event->dependency_kind != adapter->dependency_kind) {
        return UCN_ERR_STATE;
    }
    return ucn_i_coordinator_route_event(&runtime->coordinator, event,
                                         now_us);
}

static void *owner_storage(ucn_i_composition_runtime_t *runtime,
                           ucn_i_composition_module_id_t module_id)
{
    ucn_i_composition_owner_record_t *record =
        owner_record_find_mutable(runtime, module_id);

    if (record == NULL || record->storage_bytes == 0U ||
        record->storage_offset >= runtime->storage_bytes) {
        return NULL;
    }
    return (uint8_t *)runtime + record->storage_offset;
}

static bool composition_owner_is_active(
    const ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id)
{
    const ucn_i_composition_owner_record_t *record =
        owner_record_find(runtime, module_id);

    return record != NULL &&
           record->state == UCN_I_COMPOSITION_OWNER_ACTIVE;
}

static const void *module_config_pointer(
    const ucn_i_composition_start_config_t *config,
    ucn_i_composition_module_id_t module_id)
{
    switch (module_id) {
        case UCN_I_COMPOSITION_MODULE_PERSISTENCE:
            return config->persistence;
        case UCN_I_COMPOSITION_MODULE_IDENTITY:
            return config->identity;
        case UCN_I_COMPOSITION_MODULE_SECURITY:
            return config->security;
        case UCN_I_COMPOSITION_MODULE_ADMISSION:
            return config->admission;
        case UCN_I_COMPOSITION_MODULE_CAPABILITY:
            return config->capability;
        case UCN_I_COMPOSITION_MODULE_ROUTE:
            return config->route;
        case UCN_I_COMPOSITION_MODULE_FLOW:
            return config->flow;
        case UCN_I_COMPOSITION_MODULE_TRANSPORT:
            return config->transport;
        case UCN_I_COMPOSITION_MODULE_SERVICE:
            return config->service;
        case UCN_I_COMPOSITION_MODULE_REALTIME:
            return config->realtime;
        case UCN_I_COMPOSITION_MODULE_GROUP:
            return config->group;
        case UCN_I_COMPOSITION_MODULE_CLUSTER:
            return config->cluster;
        default:
            return NULL;
    }
}

static size_t module_config_bytes(
    ucn_i_composition_module_id_t module_id)
{
    switch (module_id) {
        case UCN_I_COMPOSITION_MODULE_PERSISTENCE:
            return sizeof(ucn_persistence_config_t);
        case UCN_I_COMPOSITION_MODULE_IDENTITY:
            return sizeof(ucn_i_identity_config_t);
        case UCN_I_COMPOSITION_MODULE_SECURITY:
            return sizeof(ucn_i_security_config_t);
        case UCN_I_COMPOSITION_MODULE_ADMISSION:
            return sizeof(ucn_i_admission_config_t);
        case UCN_I_COMPOSITION_MODULE_CAPABILITY:
            return sizeof(ucn_i_composition_capability_config_t);
        case UCN_I_COMPOSITION_MODULE_ROUTE:
            return sizeof(ucn_i_route_config_t);
        case UCN_I_COMPOSITION_MODULE_FLOW:
            return sizeof(ucn_i_flow_config_t);
        case UCN_I_COMPOSITION_MODULE_TRANSPORT:
            return sizeof(ucn_i_transport_config_t);
        case UCN_I_COMPOSITION_MODULE_SERVICE:
            return sizeof(ucn_i_service_config_t);
        case UCN_I_COMPOSITION_MODULE_REALTIME:
            return sizeof(ucn_i_realtime_config_t);
        case UCN_I_COMPOSITION_MODULE_GROUP:
            return sizeof(ucn_i_group_config_t);
        case UCN_I_COMPOSITION_MODULE_CLUSTER:
            return sizeof(ucn_i_cluster_config_t);
        default:
            return 0U;
    }
}

static bool module_config_identity_is_exact(
    const ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id,
    const void *module_config)
{
    const ucn_i_composition_owner_record_t *record =
        owner_record_find(runtime, module_id);
    uint32_t runtime_instance = 0U;
    uint16_t owner_instance = 0U;

    if (record == NULL || module_config == NULL) {
        return false;
    }
    switch (module_id) {
#if UCN_FEATURE_PERSISTENCE_ENABLED
        case UCN_I_COMPOSITION_MODULE_PERSISTENCE: {
            const ucn_persistence_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
#endif
        case UCN_I_COMPOSITION_MODULE_IDENTITY: {
            const ucn_i_identity_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
        case UCN_I_COMPOSITION_MODULE_SECURITY: {
            const ucn_i_security_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
        case UCN_I_COMPOSITION_MODULE_ADMISSION: {
            const ucn_i_admission_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
        case UCN_I_COMPOSITION_MODULE_CAPABILITY: {
            const ucn_i_composition_capability_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
        case UCN_I_COMPOSITION_MODULE_ROUTE: {
            const ucn_i_route_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
        case UCN_I_COMPOSITION_MODULE_FLOW: {
            const ucn_i_flow_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
        case UCN_I_COMPOSITION_MODULE_TRANSPORT: {
            const ucn_i_transport_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
        case UCN_I_COMPOSITION_MODULE_SERVICE: {
            const ucn_i_service_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
#if UCN_V6S_FEATURE_REALTIME_ENABLED
        case UCN_I_COMPOSITION_MODULE_REALTIME: {
            const ucn_i_realtime_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
#endif
#if UCN_V6S_FEATURE_GROUP_ENABLED
        case UCN_I_COMPOSITION_MODULE_GROUP: {
            const ucn_i_group_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
#endif
#if UCN_V6S_FEATURE_CLUSTER_ENABLED
        case UCN_I_COMPOSITION_MODULE_CLUSTER: {
            const ucn_i_cluster_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
#endif
        default:
            return false;
    }
    return runtime_instance == runtime->runtime_instance &&
           owner_instance == record->ref.owner_instance;
}

static bool start_config_is_valid(
    const ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_start_config_t *config)
{
    uint8_t module_id;

    if (config == NULL || config->struct_size != sizeof(*config) ||
        config->schema != UCN_I_COMPOSITION_LIFECYCLE_SCHEMA ||
        config->reserved_zero != 0U || config->foundation_storage == NULL ||
        config->foundation_storage_bytes != ucn_storage_required() ||
        config->foundation_config == NULL ||
        config->foundation_ports == NULL ||
        config->foundation_config->runtime_instance !=
            runtime->runtime_instance ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             config, sizeof(*config)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             config->foundation_storage,
                             config->foundation_storage_bytes) ||
        ucn_i_ranges_overlap(config->foundation_storage,
                             config->foundation_storage_bytes,
                             config, sizeof(*config)) ||
        ucn_i_ranges_overlap(config->foundation_storage,
                             config->foundation_storage_bytes,
                             config->foundation_config,
                             sizeof(*config->foundation_config)) ||
        ucn_i_ranges_overlap(config->foundation_storage,
                             config->foundation_storage_bytes,
                             config->foundation_ports,
                             sizeof(*config->foundation_ports)) ||
        !bytes_are_zero(config->foundation_storage,
                        config->foundation_storage_bytes)) {
        return false;
    }
    for (module_id = UCN_I_COMPOSITION_MODULE_PERSISTENCE;
         module_id <= UCN_I_COMPOSITION_MODULE_CLUSTER; ++module_id) {
        const bool selected =
            (runtime->plan.selected_module_mask &
             module_bit_from_id(module_id)) != 0U;
        const void *module_config =
            module_config_pointer(config, module_id);
        const size_t config_bytes = module_config_bytes(module_id);

        if (selected != (module_config != NULL) || config_bytes == 0U ||
            (module_config != NULL &&
             (!module_config_identity_is_exact(runtime, module_id,
                                               module_config) ||
              ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                                   module_config, config_bytes) ||
              ucn_i_ranges_overlap(config->foundation_storage,
                                   config->foundation_storage_bytes,
                                   module_config, config_bytes)))) {
            return false;
        }
    }
    if (((runtime->plan.selected_module_mask & UCN_I_COMPOSE_ROUTE) != 0U) !=
            (config->route_state_lock != NULL) ||
        ((runtime->plan.selected_module_mask & UCN_I_COMPOSE_FLOW) != 0U) !=
            (config->flow_state_lock != NULL)) {
        return false;
    }
    return true;
}

static ucn_result_t composition_enter(
    ucn_i_composition_runtime_t *runtime,
    uint32_t operation_id,
    ucn_i_callback_claim_t *claim_out)
{
    ucn_i_callback_claim_t claim;
    ucn_result_t result;

    memset(&claim, 0, sizeof(claim));
    claim.owner_instance = (uint32_t)(runtime->owner_instance_base +
                                     UCN_I_COMPOSITION_MODULE_COORDINATOR);
    claim.operation_id = operation_id;
    claim.operation_generation = UINT16_C(1);
    claim.operation_kind = UCN_I_COMPOSITION_OPERATION_LIFECYCLE;
    result = ucn_i_callback_gate_enter(&runtime->lifecycle_gate, &claim);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    *claim_out = claim;
    return UCN_OK;
}

static ucn_result_t composition_leave(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_callback_claim_t *claim,
    ucn_result_t result)
{
    if (ucn_i_callback_gate_leave(&runtime->lifecycle_gate, claim) != UCN_OK) {
        runtime->phase = UCN_I_COMPOSITION_FAULT;
        return UCN_ERR_STATE;
    }
    return result;
}

static ucn_result_t module_init(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id,
    const ucn_i_composition_start_config_t *config)
{
    void *storage = owner_storage(runtime, module_id);
    const ucn_i_composition_owner_record_t *record =
        owner_record_find(runtime, module_id);

    if (storage == NULL || record == NULL) {
        return UCN_ERR_STATE;
    }
    switch (module_id) {
        case UCN_I_COMPOSITION_MODULE_IDENTITY:
            return ucn_i_identity_owner_init(storage, config->identity);
        case UCN_I_COMPOSITION_MODULE_SECURITY:
            return ucn_i_security_owner_init(storage, config->security);
        case UCN_I_COMPOSITION_MODULE_ADMISSION:
            return ucn_i_admission_owner_init(storage, config->admission);
        case UCN_I_COMPOSITION_MODULE_CAPABILITY:
            return ucn_i_capability_owner_init(
                storage, config->capability->runtime_instance,
                config->capability->realm_id,
                config->capability->owner_instance,
                config->capability->security_owner_instance,
                config->capability->discovery_lease_us,
                config->capability->capability_lease_us,
                &config->capability->state_lock);
        case UCN_I_COMPOSITION_MODULE_ROUTE:
            return ucn_i_route_owner_init(storage, config->route,
                                          config->route_state_lock);
        case UCN_I_COMPOSITION_MODULE_FLOW:
            return ucn_i_flow_owner_init(storage, config->flow,
                                         config->flow_state_lock);
        case UCN_I_COMPOSITION_MODULE_TRANSPORT:
            return ucn_i_transport_owner_init(storage, config->transport);
        case UCN_I_COMPOSITION_MODULE_SERVICE:
            return ucn_i_service_owner_init(storage, config->service);
#if UCN_V6S_FEATURE_REALTIME_ENABLED
        case UCN_I_COMPOSITION_MODULE_REALTIME:
            return ucn_i_realtime_owner_init(storage, config->realtime);
#endif
#if UCN_V6S_FEATURE_GROUP_ENABLED
        case UCN_I_COMPOSITION_MODULE_GROUP:
            return ucn_i_group_owner_init(storage, config->group);
#endif
#if UCN_V6S_FEATURE_CLUSTER_ENABLED
        case UCN_I_COMPOSITION_MODULE_CLUSTER:
            return ucn_i_cluster_owner_init(storage, config->cluster);
#endif
        default:
            return UCN_ERR_UNSUPPORTED;
    }
}

static ucn_result_t module_destroy(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id)
{
    void *storage = owner_storage(runtime, module_id);

    if (storage == NULL) {
        return UCN_ERR_STATE;
    }
    switch (module_id) {
#if UCN_FEATURE_PERSISTENCE_ENABLED
        case UCN_I_COMPOSITION_MODULE_PERSISTENCE:
            return ucn_persistence_deinit(storage);
#endif
        case UCN_I_COMPOSITION_MODULE_IDENTITY:
            return ucn_i_identity_owner_destroy(storage);
        case UCN_I_COMPOSITION_MODULE_SECURITY:
            return ucn_i_security_owner_destroy(storage);
        case UCN_I_COMPOSITION_MODULE_ADMISSION:
            return ucn_i_admission_owner_destroy(storage);
        case UCN_I_COMPOSITION_MODULE_CAPABILITY:
            return ucn_i_capability_owner_destroy(storage);
        case UCN_I_COMPOSITION_MODULE_ROUTE:
            return ucn_i_route_owner_destroy(storage);
        case UCN_I_COMPOSITION_MODULE_FLOW:
            return ucn_i_flow_owner_destroy(storage);
        case UCN_I_COMPOSITION_MODULE_TRANSPORT:
            return ucn_i_transport_owner_destroy(storage);
        case UCN_I_COMPOSITION_MODULE_SERVICE:
            return ucn_i_service_owner_destroy(storage);
#if UCN_V6S_FEATURE_REALTIME_ENABLED
        case UCN_I_COMPOSITION_MODULE_REALTIME:
            return ucn_i_realtime_owner_destroy(storage);
#endif
#if UCN_V6S_FEATURE_GROUP_ENABLED
        case UCN_I_COMPOSITION_MODULE_GROUP:
            return ucn_i_group_owner_destroy(storage);
#endif
#if UCN_V6S_FEATURE_CLUSTER_ENABLED
        case UCN_I_COMPOSITION_MODULE_CLUSTER:
            return ucn_i_cluster_owner_reset(storage);
#endif
        default:
            return UCN_ERR_UNSUPPORTED;
    }
}

static ucn_result_t rollback_initialized(
    ucn_i_composition_runtime_t *runtime)
{
    while (runtime->initialized_count != 0U) {
        const uint8_t index = (uint8_t)(runtime->initialized_count - 1U);
        const ucn_i_composition_module_id_t module_id =
            runtime->plan.init_order[index];
        ucn_i_composition_owner_record_t *record =
            owner_record_find_mutable(runtime, module_id);
        ucn_result_t result = module_destroy(runtime, module_id);

        if (result != UCN_OK || record == NULL) {
            runtime->phase = UCN_I_COMPOSITION_FAULT;
            return UCN_ERR_STATE;
        }
        record->state = UCN_I_COMPOSITION_OWNER_RESERVED;
        --runtime->initialized_count;
    }
    if (runtime->foundation_node != NULL) {
        ucn_i_composition_owner_record_t *foundation =
            owner_record_find_mutable(
                runtime, UCN_I_COMPOSITION_MODULE_FOUNDATION);
        if (ucn_stop(runtime->foundation_node) != UCN_OK ||
            ucn_deinit(runtime->foundation_node) != UCN_OK ||
            foundation == NULL) {
            runtime->phase = UCN_I_COMPOSITION_FAULT;
            return UCN_ERR_STATE;
        }
        runtime->foundation_node = NULL;
        runtime->foundation_storage_bytes = 0U;
        foundation->state = UCN_I_COMPOSITION_OWNER_REGISTERED;
    }
    runtime->persistence_ready = 0U;
    runtime->phase = UCN_I_COMPOSITION_PREPARED;
    return UCN_OK;
}

static void publish_initialized_owners(
    ucn_i_composition_runtime_t *runtime)
{
    uint8_t index;

    for (index = 0U; index < runtime->initialized_count; ++index) {
        ucn_i_composition_owner_record_t *record = owner_record_find_mutable(
            runtime, runtime->plan.init_order[index]);
        record->state = UCN_I_COMPOSITION_OWNER_ACTIVE;
    }
    runtime->phase = UCN_I_COMPOSITION_OWNERS_PUBLISHED;
}

static void lifecycle_view_fill(
    const ucn_i_composition_runtime_t *runtime,
    uint16_t operations,
    bool made_progress,
    ucn_i_composition_lifecycle_view_t *view)
{
    uint8_t index;

    memset(view, 0, sizeof(*view));
    view->struct_size = (uint16_t)sizeof(*view);
    view->schema = UCN_I_COMPOSITION_LIFECYCLE_SCHEMA;
    view->operations = operations;
    view->phase = runtime->phase;
    view->initialized_count = runtime->initialized_count;
    view->persistence_ready = runtime->persistence_ready;
    view->foundation_ready =
        runtime->registry[0].state == UCN_I_COMPOSITION_OWNER_ACTIVE ? 1U : 0U;
    view->made_progress = made_progress ? 1U : 0U;
    for (index = 0U; index < runtime->registry_count; ++index) {
        if (runtime->registry[index].state == UCN_I_COMPOSITION_OWNER_ACTIVE) {
            ++view->active_owner_count;
        }
    }
}

ucn_result_t ucn_i_composition_start_begin(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_start_config_t *config)
{
    ucn_i_callback_claim_t claim;
    union {
        ucn_node_t *foundation;
#if UCN_FEATURE_PERSISTENCE_ENABLED
        ucn_persistence_owner_t *persistence;
#endif
    } initialized_owner;
    uint8_t index;
    ucn_result_t result;

    if (!ucn_i_composition_runtime_valid(runtime) ||
        runtime->phase != UCN_I_COMPOSITION_PREPARED ||
        !start_config_is_valid(runtime, config)) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_enter(runtime, UCN_I_COMPOSITION_OP_START_BEGIN,
                               &claim);
    if (result != UCN_OK) {
        return result;
    }
    initialized_owner.foundation = NULL;
    result = ucn_i_node_init_with_owner_instance(
        config->foundation_storage, config->foundation_storage_bytes,
        config->foundation_config, config->foundation_ports,
        runtime->registry[0].ref.owner_instance,
        &initialized_owner.foundation);
    if (result != UCN_OK) {
        return composition_leave(runtime, &claim, result);
    }
    /* Composition, not Foundation, owns the shared Wire ingress.  Set this
     * before publishing the Foundation Owner so no running C1 step can ever
     * steal a C0 obligation. / 共享 Wire 入站由 Composition 而非
     * Foundation 拥有；在发布 Foundation Owner 前置位，保证运行期 C1
     * step 不会窃取 C0 obligation。 */
    initialized_owner.foundation->external_ingress_demux = 1U;
    runtime->foundation_node = initialized_owner.foundation;
    runtime->foundation_storage_bytes =
        (uint32_t)config->foundation_storage_bytes;
    runtime->registry[0].state = UCN_I_COMPOSITION_OWNER_INITIALIZED;

    for (index = 0U; index < runtime->plan.init_count; ++index) {
        const ucn_i_composition_module_id_t module_id =
            runtime->plan.init_order[index];
        ucn_i_composition_owner_record_t *record =
            owner_record_find_mutable(runtime, module_id);

        if (module_id == UCN_I_COMPOSITION_MODULE_PERSISTENCE) {
#if UCN_FEATURE_PERSISTENCE_ENABLED
            initialized_owner.persistence = NULL;
            result = ucn_persistence_init_in_place(
                owner_storage(runtime, module_id), record->storage_bytes,
                config->persistence, &initialized_owner.persistence);
            if (result == UCN_OK &&
                initialized_owner.persistence !=
                    owner_storage(runtime, module_id)) {
                result = UCN_ERR_STATE;
            }
#else
            result = UCN_ERR_UNSUPPORTED;
#endif
        } else {
            result = module_init(runtime, module_id, config);
        }
        if (result != UCN_OK || record == NULL) {
            ucn_result_t rollback = rollback_initialized(runtime);
            return composition_leave(
                runtime, &claim,
                rollback == UCN_OK ? result : UCN_ERR_STATE);
        }
        record->state = UCN_I_COMPOSITION_OWNER_INITIALIZED;
        ++runtime->initialized_count;
    }

    if ((runtime->plan.selected_module_mask & UCN_I_COMPOSE_PERSISTENCE) !=
        0U) {
#if UCN_FEATURE_PERSISTENCE_ENABLED
        result = ucn_i_persistence_start_recovery(owner_storage(
            runtime, UCN_I_COMPOSITION_MODULE_PERSISTENCE));
#else
        result = UCN_ERR_UNSUPPORTED;
#endif
        if (result != UCN_OK) {
            ucn_result_t rollback = rollback_initialized(runtime);
            return composition_leave(
                runtime, &claim,
                rollback == UCN_OK ? result : UCN_ERR_STATE);
        }
        runtime->phase = UCN_I_COMPOSITION_RELOADING;
    } else {
        publish_initialized_owners(runtime);
    }
    return composition_leave(runtime, &claim, UCN_OK);
}

ucn_result_t ucn_i_composition_start_step(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    uint16_t operation_budget,
    ucn_i_composition_lifecycle_view_t *view_out)
{
    ucn_i_composition_lifecycle_view_t view;
    ucn_i_callback_claim_t claim;
    uint16_t operations = 0U;
    bool progress = false;
    ucn_result_t result;

    if (!ucn_i_composition_runtime_valid(runtime) || view_out == NULL ||
        operation_budget == 0U ||
        (runtime->phase != UCN_I_COMPOSITION_RELOADING &&
         runtime->phase != UCN_I_COMPOSITION_OWNERS_PUBLISHED) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             view_out, sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_enter(runtime, UCN_I_COMPOSITION_OP_START_STEP,
                               &claim);
    if (result != UCN_OK) {
        return result;
    }
    if (runtime->phase == UCN_I_COMPOSITION_RELOADING) {
#if UCN_FEATURE_PERSISTENCE_ENABLED
        ucn_persistence_owner_t *persistence = owner_storage(
            runtime, UCN_I_COMPOSITION_MODULE_PERSISTENCE);

        memset(&runtime->lifecycle_step.persistence, 0,
               sizeof(runtime->lifecycle_step.persistence));
        result = ucn_i_persistence_step(persistence, now_us,
                                        operation_budget,
                                        &runtime->lifecycle_step.persistence);
        if (result != UCN_OK ||
            runtime->lifecycle_step.persistence.domains_faulted != 0U) {
            memset(&runtime->lifecycle_step.persistence, 0,
                   sizeof(runtime->lifecycle_step.persistence));
            runtime->phase = UCN_I_COMPOSITION_FAULT;
            return composition_leave(runtime, &claim,
                                     result == UCN_OK ? UCN_ERR_STATE : result);
        }
        operations = runtime->lifecycle_step.persistence.operations_performed;
        progress = runtime->lifecycle_step.persistence.made_progress != 0U;
        if (runtime->lifecycle_step.persistence.owner_ready != 0U) {
            runtime->persistence_ready = 1U;
            publish_initialized_owners(runtime);
            progress = true;
        }
        memset(&runtime->lifecycle_step.persistence, 0,
               sizeof(runtime->lifecycle_step.persistence));
#else
        runtime->phase = UCN_I_COMPOSITION_FAULT;
        return composition_leave(runtime, &claim, UCN_ERR_UNSUPPORTED);
#endif
    }
    if (runtime->phase == UCN_I_COMPOSITION_OWNERS_PUBLISHED &&
        operations < operation_budget) {
        result = ucn_start(runtime->foundation_node, now_us);
        ++operations;
        if (result != UCN_OK) {
            if (ucn_stop(runtime->foundation_node) == UCN_OK) {
                uint8_t index;
                for (index = 0U; index < runtime->initialized_count; ++index) {
                    owner_record_find_mutable(
                        runtime, runtime->plan.init_order[index])->state =
                        UCN_I_COMPOSITION_OWNER_INITIALIZED;
                }
                runtime->phase = UCN_I_COMPOSITION_STOPPING;
            } else {
                runtime->phase = UCN_I_COMPOSITION_FAULT;
            }
            return composition_leave(runtime, &claim, result);
        }
        runtime->registry[0].state = UCN_I_COMPOSITION_OWNER_ACTIVE;
        runtime->phase = UCN_I_COMPOSITION_RUNNING;
        progress = true;
    }
    lifecycle_view_fill(runtime, operations, progress, &view);
    result = composition_leave(runtime, &claim, UCN_OK);
    if (result == UCN_OK) {
        *view_out = view;
    }
    return result;
}

ucn_result_t ucn_i_composition_stop_begin(
    ucn_i_composition_runtime_t *runtime)
{
    ucn_i_callback_claim_t claim;
    uint8_t index;
    ucn_result_t result;

    if (!ucn_i_composition_runtime_valid(runtime) ||
        (runtime->phase != UCN_I_COMPOSITION_RELOADING &&
         runtime->phase != UCN_I_COMPOSITION_OWNERS_PUBLISHED &&
         runtime->phase != UCN_I_COMPOSITION_RUNNING &&
         runtime->phase != UCN_I_COMPOSITION_FAULT)) {
        return UCN_ERR_ARGUMENT;
    }
    if (runtime->ingress_pending_kind !=
        UCN_I_COMPOSITION_INGRESS_NONE) {
        return UCN_ERR_STATE;
    }
    result = composition_enter(runtime, UCN_I_COMPOSITION_OP_STOP_BEGIN,
                               &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_stop(runtime->foundation_node);
    if (result != UCN_OK) {
        return composition_leave(runtime, &claim, result);
    }
    runtime->registry[0].state = UCN_I_COMPOSITION_OWNER_INITIALIZED;
    for (index = 0U; index < runtime->initialized_count; ++index) {
        owner_record_find_mutable(
            runtime, runtime->plan.init_order[index])->state =
            UCN_I_COMPOSITION_OWNER_INITIALIZED;
    }
    runtime->phase = UCN_I_COMPOSITION_STOPPING;
    return composition_leave(runtime, &claim, UCN_OK);
}

static ucn_result_t composition_stop_foundation_step(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    uint16_t operation_budget,
    ucn_i_composition_lifecycle_view_t *view_out)
{
#if UCN_FEATURE_PERSISTENCE_ENABLED
    ucn_step_budget_t *budget = &runtime->lifecycle_step.foundation.budget;
    ucn_step_result_t *step_result = &runtime->lifecycle_step.foundation.result;
#else
    ucn_step_budget_t budget;
    ucn_step_result_t step_result;
#endif
    ucn_i_callback_claim_t claim;
    ucn_result_t result;
    uint16_t work_done;

    result = composition_enter(runtime, UCN_I_COMPOSITION_OP_STOP_STEP,
                               &claim);
    if (result != UCN_OK) {
        return result;
    }
#if UCN_FEATURE_PERSISTENCE_ENABLED
    memset(&runtime->lifecycle_step, 0, sizeof(runtime->lifecycle_step));
    budget->struct_size = (uint16_t)sizeof(*budget);
    budget->api_version = UCN_API_VERSION;
    budget->max_work = operation_budget;
    result = ucn_i_node_stop_step(runtime->foundation_node, now_us, budget,
                                 step_result);
    work_done = step_result->work_done;
    memset(&runtime->lifecycle_step, 0, sizeof(runtime->lifecycle_step));
#else
    memset(&budget, 0, sizeof(budget));
    budget.struct_size = (uint16_t)sizeof(budget);
    budget.api_version = UCN_API_VERSION;
    budget.max_work = operation_budget;
    result = ucn_i_node_stop_step(runtime->foundation_node, now_us, &budget,
                                 &step_result);
    work_done = result == UCN_OK ? step_result.work_done : 0U;
#endif
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        lifecycle_view_fill(runtime, work_done, work_done != 0U, view_out);
    }
    return result;
}

static ucn_result_t composition_stop_destroy_step(
    ucn_i_composition_runtime_t *runtime,
    uint16_t operation_budget,
    ucn_i_composition_lifecycle_view_t *view_out)
{
    ucn_i_callback_claim_t claim;
    uint16_t operations = 0U;
    bool progress = false;
    ucn_result_t result;

    result = composition_enter(runtime, UCN_I_COMPOSITION_OP_STOP_STEP,
                               &claim);
    if (result != UCN_OK) {
        return result;
    }
    while (operations < operation_budget &&
           runtime->initialized_count != 0U) {
        const uint8_t index = (uint8_t)(runtime->initialized_count - 1U);
        const ucn_i_composition_module_id_t module_id =
            runtime->plan.init_order[index];
        ucn_i_composition_owner_record_t *record =
            owner_record_find_mutable(runtime, module_id);

        result = module_destroy(runtime, module_id);
        ++operations;
        if (result == UCN_ERR_STATE) {
            result = UCN_OK;
            break;
        }
        if (result != UCN_OK || record == NULL) {
            runtime->phase = UCN_I_COMPOSITION_FAULT;
            return composition_leave(
                runtime, &claim,
                result == UCN_OK ? UCN_ERR_STATE : result);
        }
        record->state = UCN_I_COMPOSITION_OWNER_RESERVED;
        --runtime->initialized_count;
        progress = true;
    }
    if (runtime->initialized_count == 0U &&
        operations < operation_budget) {
        result = ucn_deinit(runtime->foundation_node);
        ++operations;
        if (result != UCN_OK) {
            runtime->phase = UCN_I_COMPOSITION_FAULT;
            return composition_leave(runtime, &claim, result);
        }
        runtime->foundation_node = NULL;
        runtime->foundation_storage_bytes = 0U;
        runtime->registry[0].state = UCN_I_COMPOSITION_OWNER_REGISTERED;
        runtime->persistence_ready = 0U;
        runtime->phase = UCN_I_COMPOSITION_QUIESCENT;
        progress = true;
    }
    result = composition_leave(runtime, &claim, UCN_OK);
    if (result == UCN_OK) {
        lifecycle_view_fill(runtime, operations, progress, view_out);
    }
    return result;
}

ucn_result_t ucn_i_composition_stop_step(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    uint16_t operation_budget,
    ucn_i_composition_lifecycle_view_t *view_out)
{
#if UCN_FEATURE_PERSISTENCE_ENABLED
    ucn_i_callback_claim_t claim;
    uint16_t persistence_operations;
    bool persistence_progress;
    ucn_result_t result;
#endif

    if (!ucn_i_composition_runtime_valid(runtime) || view_out == NULL ||
        operation_budget == 0U ||
        runtime->phase != UCN_I_COMPOSITION_STOPPING ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             view_out, sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (runtime->foundation_node->lifecycle == UCN_LIFECYCLE_STOPPING) {
        return composition_stop_foundation_step(
            runtime, now_us, operation_budget, view_out);
    }
#if UCN_FEATURE_PERSISTENCE_ENABLED
    if ((runtime->plan.selected_module_mask & UCN_I_COMPOSE_PERSISTENCE) !=
        0U) {
        ucn_persistence_owner_t *persistence = owner_storage(
            runtime, UCN_I_COMPOSITION_MODULE_PERSISTENCE);

        if (persistence != NULL &&
            persistence->owner_phase == UCN_I_PERSIST_OWNER_RECOVERING) {
            result = composition_enter(
                runtime, UCN_I_COMPOSITION_OP_STOP_STEP, &claim);
            if (result != UCN_OK) {
                return result;
            }
            memset(&runtime->lifecycle_step.persistence, 0,
                   sizeof(runtime->lifecycle_step.persistence));
            result = ucn_i_persistence_step(
                persistence, now_us, operation_budget,
                &runtime->lifecycle_step.persistence);
            persistence_operations =
                runtime->lifecycle_step.persistence.operations_performed;
            persistence_progress =
                runtime->lifecycle_step.persistence.made_progress != 0U;
            memset(&runtime->lifecycle_step.persistence, 0,
                   sizeof(runtime->lifecycle_step.persistence));
            result = composition_leave(runtime, &claim, result);
            if (result == UCN_OK) {
                lifecycle_view_fill(runtime, persistence_operations,
                                    persistence_progress, view_out);
            }
            return result;
        }
    }
#else
    (void)now_us;
#endif
    return composition_stop_destroy_step(runtime, operation_budget,
                                         view_out);
}

ucn_result_t ucn_i_composition_lifecycle_view(
    const ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_lifecycle_view_t *view_out)
{
    ucn_i_composition_lifecycle_view_t view;

    if (!ucn_i_composition_runtime_valid(runtime) || view_out == NULL ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             view_out, sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    lifecycle_view_fill(runtime, 0U, false, &view);
    *view_out = view;
    return UCN_OK;
}

static bool composition_runtime_header_valid(
    const ucn_i_composition_runtime_t *runtime)
{
    return runtime != NULL &&
           pointer_is_aligned(runtime,
                              UCN_I_COMPOSITION_STORAGE_ALIGNMENT) &&
           runtime->magic == UCN_I_COMPOSITION_RUNTIME_MAGIC &&
           runtime->runtime_instance != 0U &&
           runtime->storage_bytes >= sizeof(*runtime) &&
           runtime->schema == UCN_I_COMPOSITION_RUNTIME_SCHEMA &&
           runtime->registry_count >= 2U &&
           runtime->registry_count <= UCN_I_COMPOSITION_REGISTRY_CAPACITY;
}

/* EN: Packet hot paths use the immutable Plan only as a bounded dispatch
 * index.  Re-running the complete layout/digest audit for every packet both
 * duplicates the lifecycle validation and makes Nano's call stack depend on
 * the number of compiled modules.  This guard therefore checks every value
 * that can affect a bounded lookup or dispatch, while the complete
 * ucn_i_composition_runtime_valid() audit remains mandatory at prepare/start,
 * lifecycle and explicit view boundaries.  Caller-owned opaque storage must
 * not be modified outside the Composition API.
 * 中文：报文热路径只把不可变 Plan 用作有界分派索引。每个报文都重新执行完整
 * 布局/摘要审计既重复生命周期校验，也会让 Nano 调用栈随编译模块数量增长。
 * 因此这里检查所有会影响有界查找或分派的值；prepare/start、生命周期和显式
 * View 边界仍必须执行 ucn_i_composition_runtime_valid() 完整审计。调用方持有
 * 的 opaque storage 不得绕过 Composition API 直接改写。 */
static bool composition_runtime_operational_valid(
    const ucn_i_composition_runtime_t *runtime)
{
    return composition_runtime_header_valid(runtime) &&
           runtime->owner_instance_base > UCN_I_ADAPTER_OWNER_INSTANCE &&
           runtime->owner_instance_base <=
               UINT16_MAX - UCN_I_COMPOSITION_MODULE_COORDINATOR &&
           runtime->phase >= UCN_I_COMPOSITION_PREPARED &&
           runtime->phase <= UCN_I_COMPOSITION_FAULT &&
           runtime->plan.struct_size == sizeof(runtime->plan) &&
           runtime->plan.schema == UCN_I_COMPOSITION_SCHEMA &&
           profile_is_valid(runtime->plan.profile) &&
           runtime->plan.compiled_module_mask ==
               UCN_I_COMPOSITION_COMPILED_MODULE_MASK &&
           (runtime->plan.available_module_mask &
            ~UCN_I_COMPOSE_KNOWN_MODULES) == 0U &&
           (runtime->plan.selected_module_mask &
            ~runtime->plan.available_module_mask) == 0U &&
           (runtime->plan.selected_module_mask &
            UCN_I_COMPOSE_FOUNDATION) != 0U &&
           (runtime->plan.capability_mask &
            ~UCN_I_COMPOSE_KNOWN_CAPABILITIES) == 0U &&
           (runtime->plan.capability_mask & UCN_I_CAP_STATIC_C1) != 0U &&
           runtime->plan.composition_digest != 0U &&
           runtime->plan.init_count <=
               UCN_I_COMPOSITION_INIT_ORDER_CAPACITY &&
           runtime->registry_count ==
               (uint8_t)(runtime->plan.init_count + 2U) &&
           runtime->initialized_count <= runtime->plan.init_count &&
           bytes_are_zero(runtime->plan.reserved_zero,
                          sizeof(runtime->plan.reserved_zero)) &&
           bytes_are_zero(runtime->reserved_zero,
                          sizeof(runtime->reserved_zero));
}

static bool composition_fast_path_header_valid(
    const ucn_i_composition_runtime_t *runtime)
{
    return composition_runtime_header_valid(runtime) &&
           (runtime->plan.capability_mask & UCN_I_CAP_STATIC_C1) != 0U &&
           runtime->foundation_node != NULL &&
           runtime->foundation_storage_bytes == ucn_storage_required() &&
           runtime->foundation_node->runtime_instance ==
               runtime->registry[0].ref.runtime_instance &&
           runtime->foundation_node->owner_instance ==
               runtime->registry[0].ref.owner_instance &&
           !ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                                 runtime->foundation_node,
                                 runtime->foundation_storage_bytes);
}

static bool composition_external_range(
    const ucn_i_composition_runtime_t *runtime,
    const void *object,
    size_t object_bytes)
{
    return object != NULL && object_bytes != 0U &&
           !ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                                 object, object_bytes) &&
           (runtime->foundation_node == NULL ||
            !ucn_i_ranges_overlap(runtime->foundation_node,
                                  runtime->foundation_storage_bytes,
                                  object, object_bytes));
}

static bool composition_gate_claim_is_exact(
    const ucn_i_composition_runtime_t *runtime,
    const ucn_i_callback_claim_t *claim,
    uint32_t operation_id)
{
    return claim->owner_instance ==
               (uint32_t)(runtime->owner_instance_base +
                          UCN_I_COMPOSITION_MODULE_COORDINATOR) &&
           claim->operation_id == operation_id &&
           claim->operation_generation == UINT16_C(1) &&
           claim->operation_kind ==
               UCN_I_COMPOSITION_OPERATION_LIFECYCLE;
}

static ucn_result_t composition_static_c1_enter(
    ucn_i_composition_runtime_t *runtime,
    uint32_t operation_id,
    ucn_i_callback_claim_t *claim_out)
{
    bool active = false;
    ucn_i_callback_claim_t active_claim;

    if (!composition_runtime_header_valid(runtime) || claim_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!composition_fast_path_header_valid(runtime)) {
        return UCN_ERR_STATE;
    }
    if (ucn_i_callback_gate_view(&runtime->lifecycle_gate, &active,
                                 &active_claim) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (active) {
        return UCN_ERR_STATE;
    }
    if (!composition_runtime_operational_valid(runtime)) {
        return UCN_ERR_ARGUMENT;
    }
    if (runtime->phase != UCN_I_COMPOSITION_RUNNING ||
        runtime->registry[0].state !=
            UCN_I_COMPOSITION_OWNER_ACTIVE) {
        return UCN_ERR_STATE;
    }
    return composition_enter(runtime, operation_id, claim_out);
}

static bool composition_advanced_modules_active(
    const ucn_i_composition_runtime_t *runtime)
{
    const ucn_i_composition_module_mask_t required =
        UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_ADMISSION |
        UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_CAPABILITY |
        UCN_I_COMPOSE_ROUTE | UCN_I_COMPOSE_FLOW;
    const ucn_i_composition_capability_mask_t capabilities =
        UCN_I_CAP_DYNAMIC_ADMISSION | UCN_I_CAP_ADVANCED_FLOW;

    return (runtime->plan.selected_module_mask & required) == required &&
           (runtime->plan.capability_mask & capabilities) == capabilities &&
           composition_owner_is_active(
               runtime, UCN_I_COMPOSITION_MODULE_IDENTITY) &&
           composition_owner_is_active(
               runtime, UCN_I_COMPOSITION_MODULE_ADMISSION) &&
           composition_owner_is_active(
               runtime, UCN_I_COMPOSITION_MODULE_SECURITY) &&
           composition_owner_is_active(
               runtime, UCN_I_COMPOSITION_MODULE_CAPABILITY) &&
           composition_owner_is_active(
               runtime, UCN_I_COMPOSITION_MODULE_ROUTE) &&
           composition_owner_is_active(
               runtime, UCN_I_COMPOSITION_MODULE_FLOW);
}

static ucn_result_t composition_advanced_enter(
    ucn_i_composition_runtime_t *runtime,
    uint32_t operation_id,
    ucn_i_callback_claim_t *claim_out)
{
    bool active = false;
    ucn_i_callback_claim_t active_claim;

    if (!composition_runtime_header_valid(runtime) || claim_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!composition_runtime_operational_valid(runtime) ||
        runtime->phase != UCN_I_COMPOSITION_RUNNING ||
        !composition_advanced_modules_active(runtime)) {
        return UCN_ERR_STATE;
    }
    if (ucn_i_callback_gate_view(&runtime->lifecycle_gate, &active,
                                 &active_claim) != UCN_OK || active) {
        return UCN_ERR_STATE;
    }
    return composition_enter(runtime, operation_id, claim_out);
}

static ucn_result_t control_target_module(
    uint16_t opcode,
    ucn_i_composition_module_id_t *module_out,
    uint8_t *domain_out)
{
    uint8_t domain;

    if (module_out == NULL || domain_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    /* EN: Module selection needs the same registered-opcode proof as the
     * Wire helper, but calling protocol_opcode_domain() here would repeat an
     * otherwise trivial wrapper frame on every Nano ingress path.
     * 中文：模块选择需要与 Wire helper 相同的已登记 Opcode 证明；若这里再
     * 调 protocol_opcode_domain()，会在每条 Nano ingress 路径重复一层纯
     * 包装栈帧。 */
    if (!ucn_i_protocol_opcode_valid(opcode)) {
        return UCN_ERR_MALFORMED;
    }
    domain = (uint8_t)(opcode >> 8U);
    switch (domain) {
        case UCN_I_OPCODE_IDENTITY_SECURITY:
            if (opcode >= UCN_I_OPCODE_PEER_REAUTH_HELLO) {
                *module_out = UCN_I_COMPOSITION_MODULE_SECURITY;
            } else if (opcode == UCN_I_OPCODE_ADDRESS_RENEW ||
                       opcode == UCN_I_OPCODE_ADDRESS_REVOKE) {
                *module_out = UCN_I_COMPOSITION_MODULE_IDENTITY;
            } else {
                *module_out = UCN_I_COMPOSITION_MODULE_ADMISSION;
            }
            break;
        case UCN_I_OPCODE_CAPABILITY_NEIGHBOR:
            *module_out = UCN_I_COMPOSITION_MODULE_CAPABILITY;
            break;
        case UCN_I_OPCODE_ROUTE:
            *module_out = UCN_I_COMPOSITION_MODULE_ROUTE;
            break;
        case UCN_I_OPCODE_FLOW_PATH:
            *module_out = UCN_I_COMPOSITION_MODULE_FLOW;
            break;
        case UCN_I_OPCODE_TRANSPORT:
            *module_out = UCN_I_COMPOSITION_MODULE_TRANSPORT;
            break;
        case UCN_I_OPCODE_SERVICE_OPERATION:
            *module_out = UCN_I_COMPOSITION_MODULE_SERVICE;
            break;
        case UCN_I_OPCODE_TIME:
            *module_out = UCN_I_COMPOSITION_MODULE_REALTIME;
            break;
        case UCN_I_OPCODE_GROUP:
            *module_out = UCN_I_COMPOSITION_MODULE_GROUP;
            break;
        case UCN_I_OPCODE_CLUSTER:
            *module_out = UCN_I_COMPOSITION_MODULE_CLUSTER;
            break;
        case UCN_I_OPCODE_DIAGNOSTICS:
            *module_out = UCN_I_COMPOSITION_MODULE_COORDINATOR;
            break;
        default:
            return UCN_ERR_MALFORMED;
    }
    *domain_out = domain;
    return UCN_OK;
}

static ucn_result_t composition_control_enter(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t target_module,
    uint32_t operation_id,
    ucn_i_callback_claim_t *claim_out)
{
    bool active = false;
    ucn_i_callback_claim_t active_claim;

    if (!composition_runtime_header_valid(runtime) || claim_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!composition_runtime_operational_valid(runtime) ||
        runtime->phase != UCN_I_COMPOSITION_RUNNING ||
        !composition_owner_is_active(
            runtime, UCN_I_COMPOSITION_MODULE_SECURITY)) {
        return UCN_ERR_STATE;
    }
    if (target_module != UCN_I_COMPOSITION_MODULE_COORDINATOR &&
        !composition_owner_is_active(runtime, target_module)) {
        return (runtime->plan.selected_module_mask &
                module_bit_from_id(target_module)) == 0U ?
                   UCN_ERR_UNSUPPORTED : UCN_ERR_STATE;
    }
    if (ucn_i_callback_gate_view(&runtime->lifecycle_gate, &active,
                                 &active_claim) != UCN_OK || active) {
        return UCN_ERR_STATE;
    }
    return composition_enter(runtime, operation_id, claim_out);
}

static ucn_result_t composition_bootstrap_enter(
    ucn_i_composition_runtime_t *runtime,
    uint32_t operation_id,
    ucn_i_callback_claim_t *claim_out)
{
    bool active = false;
    ucn_i_callback_claim_t active_claim;

    if (!composition_runtime_header_valid(runtime) || claim_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!composition_runtime_operational_valid(runtime) ||
        runtime->phase != UCN_I_COMPOSITION_RUNNING) {
        return UCN_ERR_STATE;
    }
    if (ucn_i_callback_gate_view(&runtime->lifecycle_gate, &active,
                                 &active_claim) != UCN_OK || active) {
        return UCN_ERR_STATE;
    }
    return composition_enter(runtime, operation_id, claim_out);
}

#if UCN_V6S_FEATURE_REALTIME_ENABLED || UCN_V6S_FEATURE_GROUP_ENABLED || \
    UCN_V6S_FEATURE_CLUSTER_ENABLED
/* EN: Optional Owners are entered through the same Coordinator gate, but an
 * operation authorizes exactly one module and one capability.  This keeps
 * LOCAL_STAMP, Static Group and Cluster Base independent even when a product
 * compiles all three archives.
 * 中文：可选 Owner 复用同一个 Coordinator 门，但每次操作只授权一个模块和
 * 一项能力。即使产品同时编译三个 archive，LOCAL_STAMP、Static Group 与
 * Cluster Base 仍保持相互独立。 */
static ucn_result_t composition_optional_enter(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id,
    ucn_i_composition_module_mask_t module_mask,
    ucn_i_composition_capability_mask_t capability,
    uint32_t operation_id,
    ucn_i_callback_claim_t *claim_out)
{
    bool active = false;
    ucn_i_callback_claim_t active_claim;

    if (!composition_runtime_header_valid(runtime) || claim_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!composition_runtime_operational_valid(runtime) ||
        runtime->phase != UCN_I_COMPOSITION_RUNNING) {
        return UCN_ERR_STATE;
    }
    if ((runtime->plan.selected_module_mask & module_mask) == 0U ||
        (runtime->plan.capability_mask & capability) == 0U) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (!composition_owner_is_active(runtime, module_id)) {
        return UCN_ERR_STATE;
    }
    if (ucn_i_callback_gate_view(&runtime->lifecycle_gate, &active,
                                 &active_claim) != UCN_OK || active) {
        return UCN_ERR_STATE;
    }
    return composition_enter(runtime, operation_id, claim_out);
}
#endif

static bool identity_view_equal(
    const ucn_i_identity_binding_view_t *left,
    const ucn_i_identity_binding_view_t *right)
{
    return left->runtime_instance == right->runtime_instance &&
           left->realm_id == right->realm_id &&
           left->address == right->address &&
           left->binding_generation == right->binding_generation &&
           left->authority_generation == right->authority_generation &&
           left->link_generation == right->link_generation &&
           left->local_deadline_us == right->local_deadline_us &&
           left->record_generation == right->record_generation &&
           left->foundation_transaction_id ==
               right->foundation_transaction_id &&
           left->witness_generation == right->witness_generation &&
           left->slot_generation == right->slot_generation &&
           left->identity_owner_instance ==
               right->identity_owner_instance &&
           left->persistence_owner_instance ==
               right->persistence_owner_instance &&
           left->schema_id == right->schema_id &&
           left->schema_version == right->schema_version &&
           memcmp(left->principal, right->principal,
                  sizeof(left->principal)) == 0 &&
           memcmp(left->body_digest, right->body_digest,
                  sizeof(left->body_digest)) == 0;
}

static bool identity_matches_security_peer(
    const ucn_i_identity_binding_view_t *identity,
    const ucn_i_security_current_facts_t *facts)
{
    return identity->address == facts->peer.address &&
           identity->binding_generation ==
               facts->peer.binding_generation &&
           identity->link_generation == facts->link_generation &&
           memcmp(identity->principal, facts->peer.principal,
                  sizeof(identity->principal)) == 0;
}

static bool security_binding_matches_route(
    const ucn_i_security_binding_t *security,
    const ucn_i_route_binding_t *route)
{
    return security->address == route->address &&
           security->binding_generation == route->generation &&
           memcmp(security->principal, route->principal,
                  sizeof(security->principal)) == 0;
}

static bool capability_matches_security(
    const ucn_i_capability_ref_t *capability,
    const ucn_i_security_current_facts_t *facts,
    const ucn_i_security_session_view_t *session,
    uint32_t runtime_instance,
    uint32_t realm_id,
    uint16_t security_owner_instance)
{
    return capability->reserved_zero == 0U &&
           capability->runtime_instance == runtime_instance &&
           capability->realm_id == realm_id &&
           capability->address == facts->peer.address &&
           capability->binding_generation ==
               facts->peer.binding_generation &&
           capability->session_generation ==
               session->session_generation &&
           capability->link_generation == facts->link_generation &&
           capability->security_owner_instance ==
               security_owner_instance &&
           memcmp(capability->principal, facts->peer.principal,
                  sizeof(capability->principal)) == 0;
}

static uint64_t earlier_deadline(uint64_t left, uint64_t right)
{
    return left < right ? left : right;
}

static ucn_result_t composition_advanced_use_preflight_locked(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_advanced_use_request_t *request,
    ucn_i_composition_advanced_use_workspace_t *workspace,
    ucn_i_composition_advanced_use_plan_t *plan_out)
{
    ucn_i_identity_owner_t *identity_owner;
    ucn_i_admission_owner_t *admission_owner;
    ucn_i_security_owner_t *security_owner;
    ucn_i_capability_owner_t *capability_owner;
    ucn_i_route_owner_t *route_owner;
    ucn_i_flow_owner_t *flow_owner;
    const ucn_i_route_soft_view_t *route;
    uint64_t deadline;
    ucn_result_t result;

    if (runtime == NULL || request == NULL || workspace == NULL ||
        plan_out == NULL || request->struct_size != sizeof(*request) ||
        request->schema != UCN_I_COMPOSITION_ADVANCED_USE_SCHEMA ||
        request->now_us == 0U || request->minimum_payload_bytes == 0U ||
        request->reserved_zero != 0U ||
        request->security.direction != UCN_I_SECURITY_ACCESS_OUTBOUND ||
        request->security.service_id != request->flow_request.service_id ||
        request->security.protocol_opcode !=
            request->flow_request.protocol_opcode ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             request, sizeof(*request)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             plan_out, sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(request, sizeof(*request), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(request, sizeof(*request), plan_out,
                             sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), plan_out,
                             sizeof(*plan_out))) {
        return UCN_ERR_ARGUMENT;
    }
    identity_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_IDENTITY);
    admission_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_ADMISSION);
    security_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    capability_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_CAPABILITY);
    route_owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_ROUTE);
    flow_owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_FLOW);
    if (identity_owner == NULL || admission_owner == NULL ||
        security_owner == NULL || capability_owner == NULL ||
        route_owner == NULL || flow_owner == NULL) {
        return UCN_ERR_STATE;
    }
    memset(workspace, 0, sizeof(*workspace));

    /* 1. Identity and Admission are checked before any authenticated use. */
    result = ucn_i_identity_binding_get(identity_owner, request->identity,
                                        request->now_us,
                                        &workspace->identity);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_admission_admitted_get(admission_owner,
                                          request->admission,
                                          request->now_us,
                                          &workspace->admission);
    if (result != UCN_OK ||
        !identity_view_equal(&workspace->identity,
                             &workspace->admission.binding) ||
        workspace->admission.link.reserved_zero != 0U ||
        workspace->admission.link.generation !=
            request->security_facts.link_generation ||
        !identity_matches_security_peer(&workspace->identity,
                                        &request->security_facts)) {
        return result == UCN_OK ? UCN_ERR_ACCESS : result;
    }

    /* 2. Security authenticates the current facts and exact ACL tuple. */
    if (request->security_facts.now_us != request->now_us) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_security_authorize(security_owner, &request->security,
                                      &request->security_facts);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_security_session_get(security_owner,
                                        request->security.session,
                                        &workspace->security);
    if (result != UCN_OK ||
        workspace->security.phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        workspace->security.session_generation !=
            workspace->admission.session_generation ||
        workspace->security.link_generation !=
            request->security_facts.link_generation ||
        workspace->security.policy_generation !=
            request->security_facts.policy_generation ||
        workspace->security.expires_at_us <= request->now_us) {
        return result == UCN_OK ? UCN_ERR_ACCESS : result;
    }

    /* 3. Capability is re-read from its Owner; the supplied Ref alone grants
     * no permission. */
    if (!capability_matches_security(
            &request->capability, &request->security_facts,
            &workspace->security, runtime->runtime_instance,
            workspace->identity.realm_id,
            request->security.session.owner_instance) ||
        request->capability.link_id != workspace->admission.link.id) {
        return UCN_ERR_ACCESS;
    }
    result = ucn_i_capability_get(capability_owner, &request->capability,
                                  request->now_us,
                                  &workspace->capability);
    if (result != UCN_OK) {
        return result;
    }
    if ((workspace->capability.record.peer.feature_bits &
         request->required_feature_bits) !=
            request->required_feature_bits ||
        workspace->capability.record.link.link_instance_generation !=
            request->security_facts.link_generation ||
        workspace->capability.record.link.processing_frame_mtu <
            request->minimum_payload_bytes) {
        return UCN_ERR_ACCESS;
    }

    /* 4. Route is resolved from current Owner state, never from a cached
     * route view supplied by the caller. */
    if (request->route_domain.realm != workspace->identity.realm_id ||
        !security_binding_matches_route(&request->security_facts.local,
                                        &request->route_domain.origin) ||
        !security_binding_matches_route(&request->security_facts.peer,
                                        &request->route_domain.destination)) {
        return UCN_ERR_ACCESS;
    }
    memset(&workspace->route_facts, 0, sizeof(workspace->route_facts));
    workspace->route_facts.now_us = request->now_us;
    workspace->route_facts.origin_session_generation =
        request->route_domain.origin_session_generation;
    workspace->route_facts.destination_binding_generation =
        request->route_domain.destination.generation;
    workspace->route_facts.link_generation = request->route_link_generation;
    result = ucn_i_route_resolve(route_owner, &request->route_domain,
                                 &workspace->route_facts,
                                 &workspace->route);
    if (result != UCN_OK) {
        return result;
    }
    if (workspace->route.kind != UCN_I_ROUTE_RESOLVED_DYNAMIC) {
        return UCN_ERR_ACCESS;
    }
    route = &workspace->route.dynamic;
    if ((route->capability_bits &
         request->required_path_capability_bits) !=
            request->required_path_capability_bits ||
        route->path_frame_mtu < request->minimum_payload_bytes ||
        route->next_hop.frame_mtu < request->minimum_payload_bytes) {
        return UCN_ERR_ACCESS;
    }

    /* 5. Flow is last because successful preflight may reserve a Sequence. */
    memset(&workspace->flow_facts, 0, sizeof(workspace->flow_facts));
    workspace->flow_facts.local = request->route_domain.origin;
    workspace->flow_facts.destination = request->route_domain.destination;
    workspace->flow_facts.next_hop = route->next_hop;
    memcpy(workspace->flow_facts.capability_digest,
           workspace->capability.digest,
           sizeof(workspace->flow_facts.capability_digest));
    workspace->flow_facts.now_us = request->now_us;
    workspace->flow_facts.route_causal_id = route->route_causal_id;
    workspace->flow_facts.route_deadline_us = route->expires_at_us;
    workspace->flow_facts.capability_deadline_us =
        workspace->capability.capability_deadline_us;
    workspace->flow_facts.route_runtime_instance = route->runtime_instance;
    workspace->flow_facts.route_generation = route->route_generation;
    workspace->flow_facts.origin_session_generation =
        request->route_domain.origin_session_generation;
    workspace->flow_facts.capability_session_generation =
        workspace->capability.reference.session_generation;
    workspace->flow_facts.capability_generation =
        workspace->capability.record.capability_generation;
    workspace->flow_facts.policy_generation =
        workspace->security.policy_generation;
    workspace->flow_facts.route_owner_instance = route->owner_instance;
    workspace->flow_facts.capability_security_owner_instance =
        workspace->capability.reference.security_owner_instance;
    result = ucn_i_flow_use_preflight(
        flow_owner, request->flow, &workspace->flow_facts,
        &request->flow_request, &workspace->flow_plan);
    if (result != UCN_OK) {
        return result;
    }

    memset(&workspace->candidate_plan, 0,
           sizeof(workspace->candidate_plan));
    workspace->candidate_plan.struct_size =
        sizeof(workspace->candidate_plan);
    workspace->candidate_plan.schema =
        UCN_I_COMPOSITION_ADVANCED_USE_SCHEMA;
    workspace->candidate_plan.runtime_instance = runtime->runtime_instance;
    deadline = earlier_deadline(workspace->identity.local_deadline_us,
                                workspace->security.expires_at_us);
    deadline = earlier_deadline(deadline,
        workspace->capability.discovery_deadline_us);
    deadline = earlier_deadline(deadline,
        workspace->capability.capability_deadline_us);
    workspace->candidate_plan.effective_deadline_us =
        earlier_deadline(deadline, route->expires_at_us);
    workspace->candidate_plan.effective_deadline_us = earlier_deadline(
        workspace->candidate_plan.effective_deadline_us,
        workspace->flow_plan.expires_at_us);
    workspace->candidate_plan.flow = request->flow;
    workspace->candidate_plan.flow_plan = workspace->flow_plan;
    workspace->candidate_plan.identity_binding_generation =
        workspace->identity.binding_generation;
    workspace->candidate_plan.admission_generation =
        workspace->admission.admission_generation;
    workspace->candidate_plan.security_session_generation =
        workspace->security.session_generation;
    workspace->candidate_plan.capability_generation =
        workspace->capability.record.capability_generation;
    workspace->candidate_plan.route_generation = route->route_generation;
    workspace->candidate_plan.route_kind = workspace->route.kind;
    *plan_out = workspace->candidate_plan;
    return UCN_OK;
}

ucn_result_t ucn_i_composition_advanced_use_preflight(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_advanced_use_request_t *request,
    ucn_i_composition_advanced_use_workspace_t *workspace,
    ucn_i_composition_advanced_use_plan_t *plan_out)
{
    ucn_i_flow_owner_t *flow_owner;
    ucn_i_callback_claim_t claim;
    ucn_result_t result;

    if (runtime == NULL || request == NULL || workspace == NULL ||
        plan_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_advanced_enter(
        runtime, UCN_I_COMPOSITION_OP_ADVANCED_USE, &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = composition_advanced_use_preflight_locked(
        runtime, request, workspace, plan_out);
    if (result != UCN_OK) {
        return composition_leave(runtime, &claim, result);
    }
    result = composition_leave(runtime, &claim, UCN_OK);
    if (result == UCN_OK) {
        return result;
    }
    flow_owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_FLOW);
    if (workspace->flow_plan.sequence != 0U && flow_owner != NULL) {
        (void)ucn_i_flow_sequence_abort(
            flow_owner, request->flow, workspace->flow_plan.sequence,
            workspace->flow_plan.reservation_generation);
    }
    return result;
}

static bool handle_is_zero(ucn_handle_t handle)
{
    return bytes_are_zero(&handle, sizeof(handle));
}

static bool digest_is_nonzero(const uint8_t *digest, size_t bytes)
{
    return digest != NULL && !bytes_are_zero(digest, bytes);
}

static void transport_binding_from_route(
    const ucn_i_route_binding_t *source,
    ucn_i_transport_binding_t *destination)
{
    memset(destination, 0, sizeof(*destination));
    destination->address = source->address;
    destination->generation = source->generation;
    memcpy(destination->principal, source->principal,
           sizeof(destination->principal));
}

static void service_binding_from_route(
    const ucn_i_route_binding_t *source,
    ucn_i_service_binding_t *destination)
{
    memset(destination, 0, sizeof(*destination));
    destination->address = source->address;
    destination->generation = source->generation;
    memcpy(destination->principal, source->principal,
           sizeof(destination->principal));
}

static bool delivery_request_semantics_valid(
    const ucn_i_composition_advanced_use_request_t *use,
    const ucn_i_composition_delivery_request_t *delivery,
    const uint8_t *body,
    size_t body_bytes)
{
    const uint8_t interaction = use->flow_request.interaction;

    if (delivery->struct_size != sizeof(*delivery) ||
        delivery->schema != UCN_I_COMPOSITION_DELIVERY_SCHEMA ||
        delivery->transport_mode > UCN_I_COMPOSITION_TRANSPORT_TRANSFER ||
        !bytes_are_zero(delivery->reserved_zero,
                        sizeof(delivery->reserved_zero)) ||
        use->flow_request.traffic_class == 0U ||
        use->flow_request.traffic_class >= UCN_I_SERVICE_CLASS_COUNT ||
        use->flow_request.origin_security == 0U ||
        interaction > UCN_INTERACTION_ERROR) {
        return false;
    }
    if (interaction == UCN_INTERACTION_REQUEST) {
        if (delivery->operation_id == 0U ||
            delivery->request_deadline_us <= use->now_us) {
            return false;
        }
    } else if (interaction == UCN_INTERACTION_ONE_WAY) {
        if (delivery->operation_id != 0U ||
            delivery->request_deadline_us != 0U) {
            return false;
        }
    } else if (delivery->operation_id == 0U ||
               delivery->request_deadline_us != 0U) {
        return false;
    }
    if (delivery->transport_mode == UCN_I_COMPOSITION_TRANSPORT_NONE) {
        return interaction == UCN_INTERACTION_REQUEST && body == NULL &&
               body_bytes == 0U &&
               bytes_are_zero(delivery->aad_digest,
                              sizeof(delivery->aad_digest)) &&
               bytes_are_zero(delivery->payload_digest,
                              sizeof(delivery->payload_digest)) &&
               bytes_are_zero(&delivery->transfer_template,
                              sizeof(delivery->transfer_template)) &&
               use->flow_request.delivery == UCN_DELIVERY_BEST_EFFORT;
    }
    if (body == NULL || body_bytes == 0U ||
        body_bytes != use->flow_request.payload_bytes ||
        !digest_is_nonzero(delivery->payload_digest,
                           sizeof(delivery->payload_digest))) {
        return false;
    }
    if (delivery->transport_mode == UCN_I_COMPOSITION_TRANSPORT_RELIABLE) {
        return body_bytes <= UCN_I_TRANSPORT_RELIABLE_BYTES &&
               body_bytes <= UINT16_MAX &&
               digest_is_nonzero(delivery->aad_digest,
                                 sizeof(delivery->aad_digest)) &&
               bytes_are_zero(&delivery->transfer_template,
                              sizeof(delivery->transfer_template)) &&
               use->flow_request.delivery == UCN_DELIVERY_RELIABLE;
    }
    return body_bytes <= UINT32_MAX &&
           bytes_are_zero(delivery->aad_digest,
                          sizeof(delivery->aad_digest)) &&
           bytes_are_zero(delivery->transfer_template.message_digest,
                          sizeof(delivery->transfer_template.message_digest)) &&
           delivery->transfer_template.parent_id == 0U &&
           delivery->transfer_template.parent_generation == 0U &&
           delivery->transfer_template.transfer_id == 0U &&
           delivery->transfer_template.service_id == 0U &&
           delivery->transfer_template.delivery == 0U &&
           delivery->transfer_template.interaction == 0U &&
           delivery->transfer_template.operation_id == 0U &&
           delivery->transfer_template.parent_kind == 0U &&
           use->flow_request.delivery != UCN_DELIVERY_LATEST;
}

static bool delivery_ranges_valid(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_advanced_use_request_t *use,
    const ucn_i_composition_delivery_request_t *delivery,
    const uint8_t *body,
    size_t body_bytes,
    ucn_i_composition_delivery_workspace_t *workspace,
    ucn_i_composition_delivery_plan_t *plan_out)
{
    if (runtime == NULL || use == NULL || delivery == NULL ||
        workspace == NULL || plan_out == NULL) {
        return false;
    }
    if (ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             use, sizeof(*use)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             delivery, sizeof(*delivery)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             plan_out, sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(use, sizeof(*use), delivery,
                             sizeof(*delivery)) ||
        ucn_i_ranges_overlap(use, sizeof(*use), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(use, sizeof(*use), plan_out,
                             sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(delivery, sizeof(*delivery), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(delivery, sizeof(*delivery), plan_out,
                             sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), plan_out,
                             sizeof(*plan_out))) {
        return false;
    }
    if (body_bytes != 0U &&
        (ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                              body, body_bytes) ||
         ucn_i_ranges_overlap(use, sizeof(*use), body, body_bytes) ||
         ucn_i_ranges_overlap(delivery, sizeof(*delivery), body,
                              body_bytes) ||
         ucn_i_ranges_overlap(workspace, sizeof(*workspace), body,
                              body_bytes) ||
         ucn_i_ranges_overlap(plan_out, sizeof(*plan_out), body,
                              body_bytes))) {
        return false;
    }
    return true;
}

static void delivery_derive_facts(
    const ucn_i_composition_advanced_use_request_t *use,
    const ucn_i_composition_delivery_request_t *delivery,
    ucn_i_composition_delivery_workspace_t *workspace)
{
    const ucn_i_composition_advanced_use_plan_t *plan =
        &workspace->candidate_plan.use;

    memset(&workspace->reliable_key, 0,
           sizeof(workspace->reliable_key));
    transport_binding_from_route(&use->route_domain.origin,
                                 &workspace->reliable_key.source);
    transport_binding_from_route(&use->route_domain.destination,
                                 &workspace->reliable_key.destination);
    workspace->reliable_key.realm = use->route_domain.realm;
    workspace->reliable_key.origin_sequence = plan->flow_plan.sequence;
    workspace->reliable_key.service_id = use->flow_request.service_id;
    workspace->reliable_key.delivery = UCN_DELIVERY_RELIABLE;

    memset(&workspace->transport_security, 0,
           sizeof(workspace->transport_security));
    workspace->transport_security.session_generation =
        workspace->use.security.session_generation;
    workspace->transport_security.key_generation =
        workspace->use.security.origin_tx_key_generation;
    workspace->transport_security.origin_security =
        use->flow_request.origin_security;

    memset(&workspace->service_key, 0, sizeof(workspace->service_key));
    service_binding_from_route(&use->route_domain.origin,
                               &workspace->service_key.client);
    service_binding_from_route(&use->route_domain.destination,
                               &workspace->service_key.server);
    workspace->service_key.security.session_generation =
        workspace->use.security.session_generation;
    workspace->service_key.security.key_generation =
        workspace->use.security.origin_tx_key_generation;
    workspace->service_key.security.policy_generation =
        workspace->use.security.policy_generation;
    workspace->service_key.security.origin_security =
        use->flow_request.origin_security;
    workspace->service_key.security.acl_authorized = 1U;
    workspace->service_key.operation_id = delivery->operation_id;
    workspace->service_key.realm = use->route_domain.realm;
    workspace->service_key.service_id = use->flow_request.service_id;
    workspace->service_key.opcode = use->flow_request.protocol_opcode;

    workspace->transfer_setup = delivery->transfer_template;
    memcpy(workspace->transfer_setup.message_digest,
           delivery->payload_digest,
           sizeof(workspace->transfer_setup.message_digest));
    workspace->transfer_setup.parent_id = plan->flow_plan.context_id;
    workspace->transfer_setup.parent_generation =
        plan->flow_plan.flow_generation;
    workspace->transfer_setup.transfer_id = plan->flow_plan.sequence;
    workspace->transfer_setup.service_id = use->flow_request.service_id;
    workspace->transfer_setup.delivery = use->flow_request.delivery;
    workspace->transfer_setup.interaction = use->flow_request.interaction;
    workspace->transfer_setup.operation_id = delivery->operation_id;
    workspace->transfer_setup.parent_kind =
        UCN_I_TRANSPORT_PARENT_KIND_FLOW;

    memset(&workspace->transfer_facts, 0,
           sizeof(workspace->transfer_facts));
    transport_binding_from_route(&use->route_domain.origin,
                                 &workspace->transfer_facts.source);
    transport_binding_from_route(&use->route_domain.destination,
                                 &workspace->transfer_facts.destination);
    workspace->transfer_facts.security = workspace->transport_security;
    memcpy(workspace->transfer_facts.parent_fingerprint,
           plan->flow_plan.flow_fingerprint,
           sizeof(workspace->transfer_facts.parent_fingerprint));
    workspace->transfer_facts.realm = use->route_domain.realm;
    workspace->transfer_facts.transport_policy_generation =
        workspace->use.security.policy_generation;
    workspace->transfer_facts.parent_kind =
        UCN_I_TRANSPORT_PARENT_KIND_FLOW;
}

static ucn_result_t delivery_rollback(
    ucn_i_flow_owner_t *flow_owner,
    ucn_i_transport_owner_t *transport_owner,
    ucn_i_service_owner_t *service_owner,
    const ucn_i_composition_delivery_plan_t *plan,
    bool qos_reserved)
{
    ucn_result_t result = UCN_OK;

    if (qos_reserved &&
        ucn_i_service_qos_remove(service_owner, plan->qos) != UCN_OK) {
        result = UCN_ERR_STATE;
    }
    if (!handle_is_zero(plan->transport)) {
        ucn_result_t item_result;
        if (plan->transport_mode == UCN_I_COMPOSITION_TRANSPORT_RELIABLE) {
            item_result = ucn_i_transport_reliable_cancel(
                transport_owner, plan->transport);
            if (item_result == UCN_OK) {
                item_result = ucn_i_transport_reliable_retire(
                    transport_owner, plan->transport);
            }
        } else {
            item_result = ucn_i_transport_transfer_abort(
                transport_owner, plan->transport);
            if (item_result == UCN_OK) {
                item_result = ucn_i_transport_transfer_retire(
                    transport_owner, plan->transport);
            }
        }
        if (item_result != UCN_OK) {
            result = UCN_ERR_STATE;
        }
    }
    if (plan->service_reserved != 0U) {
        ucn_result_t item_result = ucn_i_service_request_cancel(
            service_owner, plan->service);
        if (item_result == UCN_OK) {
            item_result = ucn_i_service_request_retire(
                service_owner, plan->service);
        }
        if (item_result != UCN_OK) {
            result = UCN_ERR_STATE;
        }
    }
    if (plan->use.flow_plan.sequence != 0U &&
        ucn_i_flow_sequence_abort(
            flow_owner, plan->use.flow, plan->use.flow_plan.sequence,
            plan->use.flow_plan.reservation_generation) != UCN_OK) {
        result = UCN_ERR_STATE;
    }
    return result;
}

ucn_result_t ucn_i_composition_delivery_prepare(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_advanced_use_request_t *use_request,
    const ucn_i_composition_delivery_request_t *delivery_request,
    const uint8_t *body,
    size_t body_bytes,
    ucn_i_composition_delivery_workspace_t *workspace,
    ucn_i_composition_delivery_plan_t *plan_out)
{



    ucn_i_callback_claim_t claim;
    ucn_result_t result;
    bool qos_reserved = false;

    if (!delivery_ranges_valid(runtime, use_request, delivery_request,
                               body, body_bytes, workspace, plan_out) ||
        !delivery_request_semantics_valid(use_request, delivery_request,
                                          body, body_bytes)) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_advanced_enter(
        runtime, UCN_I_COMPOSITION_OP_DELIVERY_PREPARE, &claim);
    if (result != UCN_OK) {
        return result;
    }
    memset(workspace, 0, sizeof(*workspace));
    workspace->transport_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_TRANSPORT);
    workspace->service_owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_SERVICE);
    workspace->flow_owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_FLOW);
    if (workspace->service_owner == NULL || workspace->flow_owner == NULL ||
        !composition_owner_is_active(
            runtime, UCN_I_COMPOSITION_MODULE_SERVICE) ||
        (runtime->plan.capability_mask & UCN_I_CAP_SERVICE) == 0U ||
        (delivery_request->transport_mode !=
             UCN_I_COMPOSITION_TRANSPORT_NONE &&
         (workspace->transport_owner == NULL ||
          !composition_owner_is_active(
              runtime, UCN_I_COMPOSITION_MODULE_TRANSPORT)))) {
        return composition_leave(runtime, &claim, UCN_ERR_UNSUPPORTED);
    }
    if ((delivery_request->transport_mode ==
             UCN_I_COMPOSITION_TRANSPORT_RELIABLE &&
         (runtime->plan.capability_mask & UCN_I_CAP_RELIABLE) == 0U) ||
        (delivery_request->transport_mode ==
             UCN_I_COMPOSITION_TRANSPORT_TRANSFER &&
         (runtime->plan.capability_mask & UCN_I_CAP_TRANSFER) == 0U)) {
        return composition_leave(runtime, &claim, UCN_ERR_UNSUPPORTED);
    }

    workspace->candidate_plan.struct_size =
        (uint16_t)sizeof(workspace->candidate_plan);
    workspace->candidate_plan.schema = UCN_I_COMPOSITION_DELIVERY_SCHEMA;
    workspace->candidate_plan.runtime_instance = runtime->runtime_instance;
    workspace->candidate_plan.transport_mode =
        delivery_request->transport_mode;
    workspace->candidate_plan.interaction =
        use_request->flow_request.interaction;
    result = composition_advanced_use_preflight_locked(
        runtime, use_request, &workspace->use,
        &workspace->candidate_plan.use);
    if (result != UCN_OK) {
        return composition_leave(runtime, &claim, result);
    }
    if ((delivery_request->transport_mode !=
             UCN_I_COMPOSITION_TRANSPORT_NONE &&
         workspace->candidate_plan.use.flow_plan.sequence == 0U) ||
        (delivery_request->transport_mode ==
             UCN_I_COMPOSITION_TRANSPORT_TRANSFER &&
         workspace->candidate_plan.use.flow_plan.flow_generation == 0U)) {
        result = UCN_ERR_STATE;
        goto rollback;
    }
    delivery_derive_facts(use_request, delivery_request, workspace);

    if (use_request->flow_request.interaction ==
        UCN_INTERACTION_REQUEST) {
        uint64_t deadline = earlier_deadline(
            delivery_request->request_deadline_us,
            workspace->candidate_plan.use.effective_deadline_us);
        result = ucn_i_service_request_begin(
            workspace->service_owner, &workspace->service_key, deadline,
            &workspace->candidate_plan.service);
        if (result != UCN_OK) {
            goto rollback;
        }
        workspace->candidate_plan.service_reserved = 1U;
    }

    if (delivery_request->transport_mode ==
        UCN_I_COMPOSITION_TRANSPORT_RELIABLE) {
        result = ucn_i_transport_reliable_begin(
            workspace->transport_owner, &workspace->reliable_key,
            &workspace->transport_security, delivery_request->aad_digest,
            delivery_request->payload_digest, body, (uint16_t)body_bytes,
            use_request->now_us, &workspace->candidate_plan.transport);
        if (result == UCN_OK) {
            result = ucn_i_transport_reliable_view(
                workspace->transport_owner, workspace->candidate_plan.transport,
                &workspace->transport_view.reliable);
        }
    } else if (delivery_request->transport_mode ==
               UCN_I_COMPOSITION_TRANSPORT_TRANSFER) {
        result = ucn_i_transport_transfer_tx_begin(
            workspace->transport_owner, &workspace->transfer_setup,
            &workspace->transfer_facts, body, (uint32_t)body_bytes,
            use_request->now_us, &workspace->candidate_plan.transport);
        if (result == UCN_OK) {
            result = ucn_i_transport_transfer_view(
                workspace->transport_owner, workspace->candidate_plan.transport,
                &workspace->transport_view.transfer);
        }
    } else {
        result = UCN_OK;
    }
    if (result != UCN_OK) {
        goto rollback;
    }

    memset(&workspace->qos_item, 0, sizeof(workspace->qos_item));
    workspace->qos_item.sendable =
        !handle_is_zero(workspace->candidate_plan.transport) ?
            workspace->candidate_plan.transport :
            workspace->candidate_plan.service;
    workspace->qos_item.deadline_us =
        workspace->candidate_plan.use.effective_deadline_us;
    if (delivery_request->transport_mode ==
        UCN_I_COMPOSITION_TRANSPORT_RELIABLE) {
        workspace->qos_item.deadline_us = earlier_deadline(
            workspace->qos_item.deadline_us,
            workspace->transport_view.reliable.deadline_us);
    } else if (delivery_request->transport_mode ==
               UCN_I_COMPOSITION_TRANSPORT_TRANSFER) {
        workspace->qos_item.deadline_us = earlier_deadline(
            workspace->qos_item.deadline_us,
            workspace->transport_view.transfer.deadline_us);
    }
    if (workspace->candidate_plan.service_reserved != 0U) {
        workspace->qos_item.deadline_us = earlier_deadline(
            workspace->qos_item.deadline_us,
            delivery_request->request_deadline_us);
    }
    workspace->qos_item.source_quota_key =
        use_request->route_domain.origin.address;
    workspace->qos_item.flow_quota_key =
        workspace->candidate_plan.use.flow_plan.flow_generation;
    workspace->qos_item.traffic_class =
        use_request->flow_request.traffic_class;
    {
        ucn_handle_t superseded;
        memset(&superseded, 0, sizeof(superseded));
        result = ucn_i_service_qos_enqueue(
            workspace->service_owner, &workspace->qos_item,
            &workspace->candidate_plan.qos, &superseded);
        if (result == UCN_OK && !handle_is_zero(superseded)) {
            result = UCN_ERR_STATE;
        }
    }
    if (result != UCN_OK) {
        goto rollback;
    }
    qos_reserved = true;
    workspace->candidate_plan.queue_deadline_us =
        workspace->qos_item.deadline_us;
    result = composition_leave(runtime, &claim, UCN_OK);
    if (result != UCN_OK) {
        (void)delivery_rollback(workspace->flow_owner, workspace->transport_owner, workspace->service_owner,
                                &workspace->candidate_plan, qos_reserved);
        return result;
    }
    *plan_out = workspace->candidate_plan;
    return UCN_OK;

rollback:
    if (delivery_rollback(workspace->flow_owner, workspace->transport_owner, workspace->service_owner,
                          &workspace->candidate_plan, qos_reserved) !=
        UCN_OK) {
        result = UCN_ERR_STATE;
    }
    return composition_leave(runtime, &claim, result);
}

static bool delivery_plan_valid(
    const ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_delivery_plan_t *plan)
{
    return plan != NULL && plan->struct_size == sizeof(*plan) &&
           plan->schema == UCN_I_COMPOSITION_DELIVERY_SCHEMA &&
           plan->runtime_instance == runtime->runtime_instance &&
           plan->transport_mode <= UCN_I_COMPOSITION_TRANSPORT_TRANSFER &&
           plan->interaction <= UCN_INTERACTION_ERROR &&
           plan->service_reserved <= 1U &&
           ((plan->interaction == UCN_INTERACTION_REQUEST) ==
            (plan->service_reserved != 0U)) &&
           bytes_are_zero(plan->reserved_zero,
                          sizeof(plan->reserved_zero)) &&
           plan->use.struct_size == sizeof(plan->use) &&
           plan->use.schema == UCN_I_COMPOSITION_ADVANCED_USE_SCHEMA &&
           plan->use.runtime_instance == runtime->runtime_instance &&
           !handle_is_zero(plan->qos) &&
           ((plan->transport_mode == UCN_I_COMPOSITION_TRANSPORT_NONE) ==
            handle_is_zero(plan->transport)) &&
           ((plan->service_reserved == 0U) ==
            handle_is_zero(plan->service));
}

ucn_result_t ucn_i_composition_delivery_cancel(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_delivery_plan_t *plan,
    ucn_i_composition_delivery_cancel_workspace_t *workspace)
{
    ucn_i_transport_owner_t *transport_owner;
    ucn_i_service_owner_t *service_owner;
    ucn_i_flow_owner_t *flow_owner;
    ucn_i_callback_claim_t claim;
    ucn_result_t result;

    if (runtime == NULL || workspace == NULL ||
        !delivery_plan_valid(runtime, plan) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             plan, sizeof(*plan)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), workspace,
                             sizeof(*workspace))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_advanced_enter(
        runtime, UCN_I_COMPOSITION_OP_DELIVERY_CANCEL, &claim);
    if (result != UCN_OK) {
        return result;
    }
    transport_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_TRANSPORT);
    service_owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_SERVICE);
    flow_owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_FLOW);
    if (service_owner == NULL || flow_owner == NULL ||
        (plan->transport_mode != UCN_I_COMPOSITION_TRANSPORT_NONE &&
         transport_owner == NULL)) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_service_qos_view(service_owner, plan->qos,
                                    &workspace->qos);
    if (result != UCN_OK || workspace->qos.selected != 0U ||
        workspace->qos.submitted != 0U ||
        memcmp(&workspace->qos.item.sendable,
               handle_is_zero(plan->transport) ? &plan->service :
                                                  &plan->transport,
               sizeof(workspace->qos.item.sendable)) != 0) {
        return composition_leave(
            runtime, &claim, result == UCN_OK ? UCN_ERR_STATE : result);
    }
    if (plan->transport_mode == UCN_I_COMPOSITION_TRANSPORT_RELIABLE) {
        result = ucn_i_transport_reliable_view(
            transport_owner, plan->transport,
            &workspace->transport.reliable);
        if (result != UCN_OK || workspace->transport.reliable.phase !=
                UCN_I_TRANSPORT_RELIABLE_READY) {
            return composition_leave(
                runtime, &claim, result == UCN_OK ? UCN_ERR_STATE : result);
        }
    } else if (plan->transport_mode ==
               UCN_I_COMPOSITION_TRANSPORT_TRANSFER) {
        result = ucn_i_transport_transfer_view(
            transport_owner, plan->transport,
            &workspace->transport.transfer);
        if (result != UCN_OK ||
            workspace->transport.transfer.phase !=
                UCN_I_TRANSPORT_TRANSFER_SETUP_PENDING) {
            return composition_leave(
                runtime, &claim, result == UCN_OK ? UCN_ERR_STATE : result);
        }
    }
    if (plan->service_reserved != 0U) {
        result = ucn_i_service_request_view(
            service_owner, plan->service, &workspace->service);
        if (result != UCN_OK ||
            workspace->service.phase != UCN_I_SERVICE_REQUEST_WAIT_SEND) {
            return composition_leave(
                runtime, &claim, result == UCN_OK ? UCN_ERR_STATE : result);
        }
    }
    result = delivery_rollback(flow_owner, transport_owner, service_owner,
                               plan, true);
    return composition_leave(runtime, &claim, result);
}

static ucn_result_t composition_durable_operation_enter(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_callback_claim_t *claim_out)
{
    bool active = false;
    ucn_i_callback_claim_t active_claim;

    if (!composition_runtime_header_valid(runtime) || claim_out == NULL ||
        !ucn_i_composition_runtime_valid(runtime) ||
        runtime->phase != UCN_I_COMPOSITION_RUNNING ||
        (runtime->plan.capability_mask &
         UCN_I_CAP_DURABLE_OPERATION) == 0U ||
        !composition_owner_is_active(
            runtime, UCN_I_COMPOSITION_MODULE_SERVICE) ||
        !composition_owner_is_active(
            runtime, UCN_I_COMPOSITION_MODULE_PERSISTENCE)) {
        return UCN_ERR_STATE;
    }
    if (ucn_i_callback_gate_view(&runtime->lifecycle_gate, &active,
                                 &active_claim) != UCN_OK || active) {
        return UCN_ERR_STATE;
    }
    return composition_enter(runtime,
                             UCN_I_COMPOSITION_OP_DURABLE_OPERATION,
                             claim_out);
}

ucn_result_t ucn_i_composition_durable_operation_begin(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_service_operation_key_t *key,
    const ucn_i_service_operation_durability_t *durability,
    uint64_t now_us,
    ucn_i_composition_durable_operation_workspace_t *workspace,
    ucn_handle_t *operation_out,
    ucn_i_service_operation_requirement_t *requirement_out)
{
    ucn_i_service_owner_t *service_owner;
    const ucn_i_composition_owner_record_t *service_record;
    ucn_i_callback_claim_t claim;
    ucn_result_t result;

    if (runtime == NULL || key == NULL || durability == NULL ||
        workspace == NULL || operation_out == NULL ||
        requirement_out == NULL || now_us == 0U ||
        durability->absolute_deadline_us <= now_us ||
        key->service.security.authenticated_replay_candidate != 0U ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             key, sizeof(*key)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             durability, sizeof(*durability)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             operation_out, sizeof(*operation_out)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             requirement_out, sizeof(*requirement_out)) ||
        ucn_i_ranges_overlap(key, sizeof(*key), durability,
                             sizeof(*durability)) ||
        ucn_i_ranges_overlap(key, sizeof(*key), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(key, sizeof(*key), operation_out,
                             sizeof(*operation_out)) ||
        ucn_i_ranges_overlap(key, sizeof(*key), requirement_out,
                             sizeof(*requirement_out)) ||
        ucn_i_ranges_overlap(durability, sizeof(*durability),
                             workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(durability, sizeof(*durability),
                             operation_out, sizeof(*operation_out)) ||
        ucn_i_ranges_overlap(durability, sizeof(*durability),
                             requirement_out, sizeof(*requirement_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace),
                             operation_out, sizeof(*operation_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace),
                             requirement_out, sizeof(*requirement_out)) ||
        ucn_i_ranges_overlap(operation_out, sizeof(*operation_out),
                             requirement_out, sizeof(*requirement_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_durable_operation_enter(runtime, &claim);
    if (result != UCN_OK) {
        return result;
    }
    service_owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_SERVICE);
    service_record = owner_record_find(
        runtime, UCN_I_COMPOSITION_MODULE_SERVICE);
    if (service_owner == NULL || service_record == NULL ||
        durability->volatile_continuation.runtime_instance !=
            runtime->runtime_instance ||
        durability->volatile_continuation.owner_instance !=
            service_record->ref.owner_instance) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_service_operation_begin(
        service_owner, key, durability, &workspace->operation);
    if (result == UCN_OK) {
        result = ucn_i_service_operation_requirement_get(
            service_owner, workspace->operation,
            &workspace->requirement);
    }
    result = composition_leave(runtime, &claim, result);
    if (result != UCN_OK) {
        return result;
    }
    *operation_out = workspace->operation;
    *requirement_out = workspace->requirement;
    return UCN_OK;
}

static ucn_result_t composition_control_dispatch_prepare_locked(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_security_authenticated_control_t *authenticated,
    const ucn_i_security_current_facts_t *current_facts,
    ucn_i_security_control_validate_workspace_t *workspace,
    ucn_i_composition_control_dispatch_plan_t *plan_out)
{
    const ucn_i_composition_owner_record_t *target_record;
    ucn_i_security_owner_t *security_owner;
    ucn_i_composition_module_id_t target_module = 0U;
    ucn_i_protocol_opcode_support_t opcode_support = 0U;
    uint8_t opcode_domain = 0U;
    ucn_result_t result;

    result = ucn_i_protocol_opcode_support(
        authenticated->protocol_opcode, &opcode_support);
    if (result == UCN_OK &&
        (opcode_support & UCN_I_OPCODE_SUPPORT_AUTH_TERMINAL) == 0U) {
        result = UCN_ERR_UNSUPPORTED;
    }
    if (result == UCN_OK) {
        result = control_target_module(authenticated->protocol_opcode,
                                       &target_module, &opcode_domain);
    }
    if (result != UCN_OK) {
        return result;
    }
    if (target_module != UCN_I_COMPOSITION_MODULE_COORDINATOR &&
        !composition_owner_is_active(runtime, target_module)) {
        return (runtime->plan.selected_module_mask &
                module_bit_from_id(target_module)) == 0U ?
                   UCN_ERR_UNSUPPORTED : UCN_ERR_STATE;
    }
    security_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    target_record = owner_record_find(runtime, target_module);
    if (security_owner == NULL ||
        authenticated->runtime_instance != runtime->runtime_instance ||
        target_record == NULL) {
        return UCN_ERR_STATE;
    }
    result = ucn_i_security_control_fact_validate(
        security_owner, authenticated, current_facts, workspace);
    if (result == UCN_OK) {
        memset(plan_out, 0, sizeof(*plan_out));
        plan_out->struct_size = (uint16_t)sizeof(*plan_out);
        plan_out->schema = UCN_I_COMPOSITION_CONTROL_SCHEMA;
        plan_out->runtime_instance = runtime->runtime_instance;
        plan_out->target = target_record->ref;
        plan_out->authenticated = *authenticated;
        plan_out->opcode_domain = opcode_domain;
        plan_out->target_module = (uint8_t)target_module;
    }
    return result;
}

ucn_result_t ucn_i_composition_control_dispatch_prepare(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_security_authenticated_control_t *authenticated,
    const ucn_i_security_current_facts_t *current_facts,
    ucn_i_security_control_validate_workspace_t *workspace,
    ucn_i_composition_control_dispatch_plan_t *plan_out)
{
    ucn_i_composition_module_id_t target_module;
    ucn_i_callback_claim_t claim;
    uint8_t opcode_domain;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || authenticated == NULL ||
        current_facts == NULL || workspace == NULL || plan_out == NULL ||
        (authenticated->body == NULL && authenticated->body_bytes != 0U) ||
        (authenticated->body_bytes != 0U &&
         (!composition_external_range(runtime, authenticated->body,
                                      authenticated->body_bytes) ||
          ucn_i_ranges_overlap(authenticated->body,
                               authenticated->body_bytes, current_facts,
                               sizeof(*current_facts)) ||
          ucn_i_ranges_overlap(authenticated->body,
                               authenticated->body_bytes, workspace,
                               sizeof(*workspace)) ||
          ucn_i_ranges_overlap(authenticated->body,
                               authenticated->body_bytes, plan_out,
                               sizeof(*plan_out)))) ||
        !composition_external_range(runtime, authenticated,
                                    sizeof(*authenticated)) ||
        !composition_external_range(runtime, current_facts,
                                    sizeof(*current_facts)) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        !composition_external_range(runtime, plan_out, sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(authenticated, sizeof(*authenticated),
                             current_facts, sizeof(*current_facts)) ||
        ucn_i_ranges_overlap(authenticated, sizeof(*authenticated),
                             workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(authenticated, sizeof(*authenticated),
                             plan_out, sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(current_facts, sizeof(*current_facts),
                             workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(current_facts, sizeof(*current_facts),
                             plan_out, sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), plan_out,
                             sizeof(*plan_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = control_target_module(authenticated->protocol_opcode,
                                   &target_module, &opcode_domain);
    if (result != UCN_OK) {
        return result;
    }
    result = composition_control_enter(runtime, target_module,
                                       UCN_I_COMPOSITION_OP_CONTROL_PREPARE,
                                       &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = composition_control_dispatch_prepare_locked(
        runtime, authenticated, current_facts, workspace, plan_out);
    (void)opcode_domain;
    return composition_leave(runtime, &claim, result);
}

static ucn_result_t composition_control_dispatch_finish_locked(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_dispatch_plan_t *plan,
    const ucn_i_security_current_facts_t *current_facts,
    ucn_i_security_control_validate_workspace_t *workspace,
    bool consumed)
{
    const ucn_i_composition_owner_record_t *target_record;
    ucn_i_security_owner_t *security_owner;
    const ucn_i_composition_module_id_t target_module =
        (ucn_i_composition_module_id_t)plan->target_module;
    ucn_result_t result;

    security_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    target_record = owner_record_find(runtime, target_module);
    if (security_owner == NULL || target_record == NULL ||
        memcmp(&plan->target, &target_record->ref,
               sizeof(plan->target)) != 0) {
        return UCN_ERR_STATE;
    }
    result = ucn_i_security_control_fact_validate(
        security_owner, &plan->authenticated, current_facts, workspace);
    if (result == UCN_OK) {
        result = consumed ?
            ucn_i_security_control_replay_commit(
                security_owner, &(plan->authenticated.replay), current_facts) :
            ucn_i_security_replay_abort(
                security_owner, &(plan->authenticated.replay));
    } else {
        (void)ucn_i_security_replay_abort(
            security_owner, &(plan->authenticated.replay));
    }
    return result;
}

ucn_result_t ucn_i_composition_control_dispatch_finish(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_dispatch_plan_t *plan,
    const ucn_i_security_current_facts_t *current_facts,
    ucn_i_security_control_validate_workspace_t *workspace,
    bool consumed)
{
    ucn_i_composition_module_id_t target_module;
    ucn_i_callback_claim_t claim;
    uint8_t opcode_domain;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || plan == NULL ||
        current_facts == NULL || workspace == NULL ||
        plan->struct_size != sizeof(*plan) ||
        plan->schema != UCN_I_COMPOSITION_CONTROL_SCHEMA ||
        plan->runtime_instance != runtime->runtime_instance ||
        !bytes_are_zero(plan->reserved_zero, sizeof(plan->reserved_zero)) ||
        !composition_external_range(runtime, plan, sizeof(*plan)) ||
        !composition_external_range(runtime, current_facts,
                                    sizeof(*current_facts)) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), current_facts,
                             sizeof(*current_facts)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(current_facts, sizeof(*current_facts),
                             workspace, sizeof(*workspace))) {
        return UCN_ERR_ARGUMENT;
    }
    result = control_target_module(plan->authenticated.protocol_opcode,
                                   &target_module, &opcode_domain);
    if (result != UCN_OK || plan->target_module != (uint8_t)target_module ||
        plan->opcode_domain != opcode_domain) {
        return result == UCN_OK ? UCN_ERR_STATE : result;
    }
    result = composition_control_enter(runtime, target_module,
                                       UCN_I_COMPOSITION_OP_CONTROL_FINISH,
                                       &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = composition_control_dispatch_finish_locked(
        runtime, plan, current_facts, workspace, consumed);
    return composition_leave(runtime, &claim, result);
}

static bool route_ingress_matches_security(
    const ucn_i_route_link_ref_t *ingress,
    const ucn_i_security_current_facts_t *facts)
{
    return ingress->reserved_zero == 0U && ingress->link_id != 0U &&
           ingress->link_id != UINT16_MAX && ingress->link_generation != 0U &&
           ingress->link_generation == facts->link_generation &&
           ingress->cost != 0U && ingress->frame_mtu != 0U &&
           security_binding_matches_route(&facts->peer, &ingress->peer);
}

static bool route_control_semantics_valid(
    const ucn_i_security_authenticated_control_t *authenticated)
{
    uint8_t expected_interaction;

    switch (authenticated->protocol_opcode) {
        case UCN_I_OPCODE_ROUTE_RREQ:
            expected_interaction = UCN_INTERACTION_REQUEST;
            break;
        case UCN_I_OPCODE_ROUTE_RREP:
            expected_interaction = UCN_INTERACTION_RESULT;
            break;
        case UCN_I_OPCODE_ROUTE_RERR:
            expected_interaction = UCN_INTERACTION_ERROR;
            break;
        default:
            return false;
    }
    return authenticated->traffic_class == UCN_TRAFFIC_Q0 &&
           authenticated->delivery_guarantee == UCN_DELIVERY_BEST_EFFORT &&
           authenticated->interaction_role == expected_interaction &&
           authenticated->payload_kind == UCN_I_PAYLOAD_CONTROL &&
           authenticated->origin_security != UCN_I_ORIGIN_O0 &&
           authenticated->hop_limit == 1U;
}

ucn_result_t ucn_i_composition_diagnostic_control_handle(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_dispatch_plan_t *plan,
    const ucn_i_security_current_facts_t *current_facts,
    ucn_i_composition_diagnostic_control_workspace_t *workspace,
    ucn_i_composition_diagnostic_control_result_t *result_out)
{
    const ucn_i_composition_owner_record_t *coordinator_record;
    const ucn_i_security_authenticated_control_t *authenticated;
    ucn_i_security_owner_t *security_owner;
    ucn_i_callback_claim_t claim;
    uint8_t expected_role;
    uint16_t response_opcode;
    bool business_effect_applied = false;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || plan == NULL ||
        current_facts == NULL || workspace == NULL || result_out == NULL ||
        plan->struct_size != sizeof(*plan) ||
        plan->schema != UCN_I_COMPOSITION_CONTROL_SCHEMA ||
        plan->runtime_instance != runtime->runtime_instance ||
        plan->target_module != UCN_I_COMPOSITION_MODULE_COORDINATOR ||
        plan->opcode_domain != UCN_I_OPCODE_DIAGNOSTICS ||
        !bytes_are_zero(plan->reserved_zero, sizeof(plan->reserved_zero)) ||
        !composition_external_range(runtime, plan, sizeof(*plan)) ||
        !composition_external_range(runtime, current_facts,
                                    sizeof(*current_facts)) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        !composition_external_range(runtime, result_out,
                                    sizeof(*result_out)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), current_facts,
                             sizeof(*current_facts)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), result_out,
                             sizeof(*result_out)) ||
        ucn_i_ranges_overlap(current_facts, sizeof(*current_facts),
                             workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(current_facts, sizeof(*current_facts),
                             result_out, sizeof(*result_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), result_out,
                             sizeof(*result_out))) {
        return UCN_ERR_ARGUMENT;
    }
    authenticated = &plan->authenticated;
    if (authenticated->protocol_opcode == UCN_I_OPCODE_DIAGNOSTIC_PING) {
        expected_role = UCN_INTERACTION_REQUEST;
        response_opcode = UCN_I_OPCODE_DIAGNOSTIC_PONG;
    } else if (authenticated->protocol_opcode ==
               UCN_I_OPCODE_DIAGNOSTIC_PONG) {
        expected_role = UCN_INTERACTION_RESULT;
        response_opcode = 0U;
    } else {
        return UCN_ERR_UNSUPPORTED;
    }
    if (authenticated->body == NULL ||
        authenticated->body_bytes != UCN_I_DIAGNOSTIC_ECHO_BYTES ||
        authenticated->traffic_class != UCN_TRAFFIC_Q0 ||
        authenticated->delivery_guarantee != UCN_DELIVERY_BEST_EFFORT ||
        authenticated->interaction_role != expected_role ||
        authenticated->payload_kind != UCN_I_PAYLOAD_DIAGNOSTIC ||
        authenticated->origin_security == UCN_I_ORIGIN_O0 ||
        !composition_external_range(runtime, authenticated->body,
                                    authenticated->body_bytes) ||
        ucn_i_ranges_overlap(authenticated->body,
                             authenticated->body_bytes, plan,
                             sizeof(*plan)) ||
        ucn_i_ranges_overlap(authenticated->body,
                             authenticated->body_bytes, current_facts,
                             sizeof(*current_facts)) ||
        ucn_i_ranges_overlap(authenticated->body,
                             authenticated->body_bytes, workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(authenticated->body,
                             authenticated->body_bytes, result_out,
                             sizeof(*result_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_control_enter(
        runtime, UCN_I_COMPOSITION_MODULE_COORDINATOR,
        UCN_I_COMPOSITION_OP_CONTROL_PREPARE, &claim);
    if (result != UCN_OK) {
        return result;
    }
    security_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    coordinator_record = owner_record_find(
        runtime, UCN_I_COMPOSITION_MODULE_COORDINATOR);
    if (security_owner == NULL || coordinator_record == NULL ||
        !owner_ref_equal(&plan->target, &coordinator_record->ref)) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_security_control_fact_validate(
        security_owner, authenticated, current_facts, &workspace->validate);
    if (result == UCN_OK) {
        result = ucn_i_diagnostic_echo_decode(
            authenticated->body, &workspace->echo);
    }
    if (result == UCN_OK) {
        memset(&workspace->candidate, 0, sizeof(workspace->candidate));
        workspace->candidate.struct_size =
            (uint16_t)sizeof(workspace->candidate);
        workspace->candidate.schema =
            UCN_I_COMPOSITION_DIAGNOSTIC_CONTROL_SCHEMA;
        workspace->candidate.runtime_instance = runtime->runtime_instance;
        workspace->candidate.transaction_id =
            authenticated->transaction_id;
        workspace->candidate.nonce = workspace->echo.nonce;
        workspace->candidate.opcode = authenticated->protocol_opcode;
        workspace->candidate.response_opcode = response_opcode;
        business_effect_applied = true;
        result = ucn_i_security_control_replay_complete(
            security_owner, &(authenticated->replay), current_facts);
    }
    if (result != UCN_OK && business_effect_applied) {
        composition_mark_fault(runtime);
        result = UCN_ERR_IN_DOUBT;
    }
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *result_out = workspace->candidate;
    }
    return result;
}

ucn_result_t ucn_i_composition_route_control_handle(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_dispatch_plan_t *plan,
    const ucn_i_security_current_facts_t *current_facts,
    const ucn_i_route_link_ref_t *ingress,
    uint64_t now_us,
    ucn_i_composition_route_control_workspace_t *workspace,
    ucn_i_composition_route_control_result_t *result_out)
{
    const ucn_i_composition_owner_record_t *route_record;
    const ucn_i_security_authenticated_control_t *authenticated;
    ucn_i_security_owner_t *security_owner;
    ucn_i_route_owner_t *route_owner;
    ucn_i_callback_claim_t claim;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || plan == NULL ||
        current_facts == NULL || ingress == NULL || now_us == 0U ||
        workspace == NULL || result_out == NULL ||
        plan->struct_size != sizeof(*plan) ||
        plan->schema != UCN_I_COMPOSITION_CONTROL_SCHEMA ||
        plan->runtime_instance != runtime->runtime_instance ||
        plan->target_module != UCN_I_COMPOSITION_MODULE_ROUTE ||
        plan->opcode_domain != UCN_I_OPCODE_ROUTE ||
        current_facts->now_us != now_us ||
        !bytes_are_zero(plan->reserved_zero, sizeof(plan->reserved_zero)) ||
        !composition_external_range(runtime, plan, sizeof(*plan)) ||
        !composition_external_range(runtime, current_facts,
                                    sizeof(*current_facts)) ||
        !composition_external_range(runtime, ingress, sizeof(*ingress)) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        !composition_external_range(runtime, result_out,
                                    sizeof(*result_out)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), current_facts,
                             sizeof(*current_facts)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), ingress,
                             sizeof(*ingress)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), result_out,
                             sizeof(*result_out)) ||
        ucn_i_ranges_overlap(current_facts, sizeof(*current_facts), ingress,
                             sizeof(*ingress)) ||
        ucn_i_ranges_overlap(current_facts, sizeof(*current_facts), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(current_facts, sizeof(*current_facts), result_out,
                             sizeof(*result_out)) ||
        ucn_i_ranges_overlap(ingress, sizeof(*ingress), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(ingress, sizeof(*ingress), result_out,
                             sizeof(*result_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), result_out,
                             sizeof(*result_out))) {
        return UCN_ERR_ARGUMENT;
    }
    authenticated = &plan->authenticated;
    if ((authenticated->body == NULL && authenticated->body_bytes != 0U) ||
        (authenticated->body_bytes != 0U &&
         (!composition_external_range(runtime, authenticated->body,
                                      authenticated->body_bytes) ||
          ucn_i_ranges_overlap(authenticated->body,
                               authenticated->body_bytes, plan,
                               sizeof(*plan)) ||
          ucn_i_ranges_overlap(authenticated->body,
                               authenticated->body_bytes, current_facts,
                               sizeof(*current_facts)) ||
          ucn_i_ranges_overlap(authenticated->body,
                               authenticated->body_bytes, ingress,
                               sizeof(*ingress)) ||
          ucn_i_ranges_overlap(authenticated->body,
                               authenticated->body_bytes, workspace,
                               sizeof(*workspace)) ||
          ucn_i_ranges_overlap(authenticated->body,
                               authenticated->body_bytes, result_out,
                               sizeof(*result_out)))) ||
        !route_control_semantics_valid(authenticated) ||
        !route_ingress_matches_security(ingress, current_facts)) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_control_enter(
        runtime, UCN_I_COMPOSITION_MODULE_ROUTE,
        UCN_I_COMPOSITION_OP_ROUTE_CONTROL, &claim);
    if (result != UCN_OK) {
        return result;
    }
    route_owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_ROUTE);
    security_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    route_record = owner_record_find(
        runtime, UCN_I_COMPOSITION_MODULE_ROUTE);
    if (route_owner == NULL || security_owner == NULL ||
        route_record == NULL ||
        !owner_ref_equal(&plan->target, &route_record->ref) ||
        authenticated->runtime_instance != runtime->runtime_instance ||
        authenticated->realm_id != route_owner->realm ||
        !security_binding_matches_route(&current_facts->local,
                                        &route_owner->local)) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_security_control_fact_validate(
        security_owner, authenticated, current_facts,
        &workspace->validate);
    if (result != UCN_OK) {
        return composition_leave(runtime, &claim, result);
    }
    memset(&workspace->candidate, 0, sizeof(workspace->candidate));
    workspace->candidate.struct_size =
        (uint16_t)sizeof(workspace->candidate);
    workspace->candidate.schema = UCN_I_COMPOSITION_ROUTE_CONTROL_SCHEMA;
    workspace->candidate.runtime_instance = runtime->runtime_instance;
    workspace->candidate.transaction_id = authenticated->transaction_id;
    workspace->candidate.opcode = authenticated->protocol_opcode;
    switch (authenticated->protocol_opcode) {
        case UCN_I_OPCODE_ROUTE_RREQ:
            result = ucn_i_route_control_request_decode(
                authenticated->body, authenticated->body_bytes,
                route_owner->address_width, &workspace->message.rreq);
            if (result != UCN_OK) {
                break;
            }
            result = ucn_i_route_on_rreq(
                route_owner, &workspace->message.rreq, ingress, now_us,
                &workspace->candidate.action.request);
            if (result == UCN_OK) {
                workspace->candidate.kind =
                    UCN_I_COMPOSITION_ROUTE_CONTROL_RREQ;
            }
            break;
        case UCN_I_OPCODE_ROUTE_RREP:
            result = ucn_i_route_control_reply_decode(
                authenticated->body, authenticated->body_bytes,
                route_owner->address_width, &workspace->message.rrep,
                &workspace->candidate.reporter_route_generation);
            if (result != UCN_OK) {
                break;
            }
            /* Only a zero-hop reply is a claim by the target itself.
             * Relays authenticate the immediate neighbor, not the original
             * target Principal. 零跳才要求报告方就是目标；中继不冒充端点。 */
            if (workspace->message.rrep.payload.hop_count == 0U &&
                (workspace->message.rrep.key.target_address !=
                    current_facts->peer.address ||
                 workspace->message.rrep.payload.
                        destination_binding_generation !=
                    authenticated->source_binding_generation ||
                memcmp(workspace->message.rrep.payload.
                           destination_principal,
                       current_facts->peer.principal,
                       sizeof(current_facts->peer.principal)) != 0)) {
                result = UCN_ERR_SECURITY;
                break;
            }
            result = ucn_i_route_on_control_rrep(
                route_owner, &workspace->message.rrep,
                workspace->candidate.reporter_route_generation, ingress, now_us,
                &workspace->candidate.action.reply);
            if (result == UCN_OK) {
                workspace->candidate.kind =
                    UCN_I_COMPOSITION_ROUTE_CONTROL_RREP;
            }
            break;
        case UCN_I_OPCODE_ROUTE_RERR:
            if (authenticated->body_bytes != UCN_I_ROUTE_RERR_BYTES) {
                result = UCN_ERR_MALFORMED;
                break;
            }
            result = ucn_i_route_rerr_decode(
                authenticated->body, &workspace->message.rerr);
            if (result != UCN_OK) {
                break;
            }
            if (workspace->message.rerr.reporter_address !=
                    current_facts->peer.address ||
                workspace->message.rerr.reporter_binding_generation !=
                    current_facts->peer.binding_generation ||
                memcmp(workspace->message.rerr.reporter_principal,
                       current_facts->peer.principal,
                       sizeof(current_facts->peer.principal)) != 0) {
                result = UCN_ERR_SECURITY;
                break;
            }
            result = ucn_i_route_on_wire_rerr(
                route_owner, &workspace->message.rerr,
                &workspace->candidate.invalidated);
            if (result == UCN_OK) {
                workspace->candidate.kind =
                    UCN_I_COMPOSITION_ROUTE_CONTROL_RERR;
            }
            break;
        default:
            result = UCN_ERR_UNSUPPORTED;
            break;
    }
    if (result == UCN_OK) {
        result = ucn_i_security_control_replay_complete(
            security_owner, &(authenticated->replay), current_facts);
        if (result != UCN_OK) {
            composition_mark_fault(runtime);
            result = UCN_ERR_IN_DOUBT;
        }
    }
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *result_out = workspace->candidate;
    }
    return result;
}

static bool capability_control_contract(
    const ucn_i_security_authenticated_control_t *authenticated,
    size_t *body_bytes_out,
    ucn_i_composition_capability_control_kind_t *kind_out)
{
    size_t body_bytes;
    ucn_i_composition_capability_control_kind_t kind;
    uint8_t delivery;
    uint8_t role;

    if (authenticated == NULL || body_bytes_out == NULL ||
        kind_out == NULL) {
        return false;
    }
    switch (authenticated->protocol_opcode) {
        case UCN_I_OPCODE_NEIGHBOR_HELLO:
            body_bytes = UCN_I_CAPABILITY_SUMMARY_BYTES;
            kind = UCN_I_COMPOSITION_CAPABILITY_CONTROL_NEIGHBOR_HELLO;
            delivery = UCN_DELIVERY_BEST_EFFORT;
            role = UCN_INTERACTION_ONE_WAY;
            break;
        case UCN_I_OPCODE_NEIGHBOR_KEEPALIVE:
            body_bytes = UCN_I_CAPABILITY_SUMMARY_BYTES;
            kind = UCN_I_COMPOSITION_CAPABILITY_CONTROL_NEIGHBOR_KEEPALIVE;
            delivery = UCN_DELIVERY_BEST_EFFORT;
            role = UCN_INTERACTION_ONE_WAY;
            break;
        case UCN_I_OPCODE_CAPABILITY_ADVERTISE:
            body_bytes = UCN_I_CAPABILITY_RECORD_BYTES;
            kind = UCN_I_COMPOSITION_CAPABILITY_CONTROL_ADVERTISE;
            delivery = UCN_DELIVERY_BEST_EFFORT;
            role = UCN_INTERACTION_ONE_WAY;
            break;
        case UCN_I_OPCODE_CAPABILITY_SELECT:
            body_bytes = UCN_I_PROFILE_SELECT_BYTES;
            kind = UCN_I_COMPOSITION_CAPABILITY_CONTROL_SELECT;
            delivery = UCN_DELIVERY_BEST_EFFORT;
            role = UCN_INTERACTION_REQUEST;
            break;
        case UCN_I_OPCODE_CAPABILITY_ACK:
            body_bytes = UCN_I_PROFILE_ACK_BYTES;
            kind = UCN_I_COMPOSITION_CAPABILITY_CONTROL_ACK;
            delivery = UCN_DELIVERY_BEST_EFFORT;
            role = UCN_INTERACTION_RESULT;
            break;
        case UCN_I_OPCODE_CAPABILITY_REVOKE:
            body_bytes = UCN_I_CAPABILITY_QUERY_BYTES;
            kind = UCN_I_COMPOSITION_CAPABILITY_CONTROL_REVOKE;
            delivery = UCN_DELIVERY_BEST_EFFORT;
            role = UCN_INTERACTION_ONE_WAY;
            break;
        case UCN_I_OPCODE_CAPABILITY_QUERY:
            body_bytes = UCN_I_CAPABILITY_QUERY_BYTES;
            kind = UCN_I_COMPOSITION_CAPABILITY_CONTROL_QUERY;
            delivery = UCN_DELIVERY_BEST_EFFORT;
            role = UCN_INTERACTION_REQUEST;
            break;
        default:
            return false;
    }
    if (authenticated->traffic_class != UCN_TRAFFIC_Q2 ||
        authenticated->delivery_guarantee != delivery ||
        authenticated->interaction_role != role ||
        authenticated->payload_kind != UCN_I_PAYLOAD_CONTROL ||
        authenticated->origin_security == UCN_I_ORIGIN_O0 ||
        authenticated->hop_limit != 1U ||
        authenticated->body_bytes != body_bytes) {
        return false;
    }
    *body_bytes_out = body_bytes;
    *kind_out = kind;
    return true;
}

static bool capability_control_context_valid(
    const ucn_i_composition_capability_control_context_t *context,
    ucn_i_composition_capability_control_kind_t kind)
{
    if (kind != UCN_I_COMPOSITION_CAPABILITY_CONTROL_SELECT &&
        kind != UCN_I_COMPOSITION_CAPABILITY_CONTROL_ACK) {
        return context == NULL;
    }
    if (context == NULL || context->struct_size != sizeof(*context) ||
        context->schema != UCN_I_COMPOSITION_CAPABILITY_CONTROL_SCHEMA ||
        context->local_record_valid > 1U ||
        context->requirements_valid > 1U ||
        context->expected_select_valid > 1U ||
        context->expected_transcript_valid > 1U) {
        return false;
    }
    if (kind == UCN_I_COMPOSITION_CAPABILITY_CONTROL_SELECT) {
        return context->local_record_valid != 0U &&
               context->requirements_valid != 0U;
    }
    return context->expected_select_valid != 0U &&
           context->expected_transcript_valid != 0U;
}

static void capability_peer_view_build(
    const ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_rx_plan_t *rx_plan,
    const ucn_i_security_session_view_t *session,
    ucn_i_authenticated_peer_view_t *peer)
{
    memset(peer, 0, sizeof(*peer));
    peer->runtime_instance = runtime->runtime_instance;
    peer->realm_id = rx_plan->dispatch.authenticated.realm_id;
    peer->address = rx_plan->facts.peer.address;
    peer->binding_generation = rx_plan->facts.peer.binding_generation;
    peer->session_generation = session->session_generation;
    peer->link_id = (uint32_t)rx_plan->link_slot + UINT32_C(1);
    peer->link_generation = rx_plan->link_instance_generation;
    peer->expires_at_us = session->expires_at_us;
    peer->security_owner_instance =
        rx_plan->dispatch.authenticated.security_owner_instance;
    memcpy(peer->principal, rx_plan->facts.peer.principal,
           sizeof(peer->principal));
}

static void capability_ref_from_peer(
    const ucn_i_authenticated_peer_view_t *peer,
    ucn_i_capability_ref_t *reference)
{
    memset(reference, 0, sizeof(*reference));
    reference->runtime_instance = peer->runtime_instance;
    reference->realm_id = peer->realm_id;
    reference->address = peer->address;
    reference->binding_generation = peer->binding_generation;
    reference->session_generation = peer->session_generation;
    reference->link_id = peer->link_id;
    reference->link_generation = peer->link_generation;
    reference->security_owner_instance = peer->security_owner_instance;
    memcpy(reference->principal, peer->principal,
           sizeof(reference->principal));
}

static bool security_session_matches_control_rx(
    const ucn_i_security_session_view_t *session,
    const ucn_i_composition_control_rx_plan_t *rx_plan,
    uint64_t now_us)
{
    const ucn_i_security_authenticated_control_t *authenticated =
        &rx_plan->dispatch.authenticated;

    return session->phase == UCN_I_SECURITY_SESSION_ACTIVE &&
           session->reserved_zero == 0U &&
           session->session_generation ==
               authenticated->session.session_generation &&
           session->link_generation == rx_plan->link_instance_generation &&
           session->link_generation == rx_plan->facts.link_generation &&
           session->policy_generation == rx_plan->facts.policy_generation &&
           session->expires_at_us > now_us &&
           session->peer.address == rx_plan->facts.peer.address &&
           session->peer.binding_generation ==
               rx_plan->facts.peer.binding_generation &&
           memcmp(session->peer.principal, rx_plan->facts.peer.principal,
                  sizeof(session->peer.principal)) == 0;
}

ucn_result_t ucn_i_composition_capability_control_handle(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_rx_plan_t *rx_plan,
    uint64_t now_us,
    ucn_i_composition_capability_control_workspace_t *workspace,
    ucn_i_composition_capability_control_result_t *result_out)
{
    return ucn_i_composition_capability_control_handle_with_context(
        runtime, rx_plan, now_us, NULL, workspace, result_out);
}

ucn_result_t ucn_i_composition_capability_control_handle_with_context(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_rx_plan_t *rx_plan,
    uint64_t now_us,
    const ucn_i_composition_capability_control_context_t *context,
    ucn_i_composition_capability_control_workspace_t *workspace,
    ucn_i_composition_capability_control_result_t *result_out)
{
    const ucn_i_composition_control_dispatch_plan_t *plan;
    const ucn_i_security_authenticated_control_t *authenticated;
    const ucn_i_composition_owner_record_t *capability_record;
    ucn_i_security_owner_t *security_owner;
    ucn_i_capability_owner_t *capability_owner;
    ucn_i_callback_claim_t claim;
    uint32_t current_link_instance = 0U;
    uint16_t current_link_mtu = 0U;
    size_t expected_body_bytes = 0U;
    ucn_i_composition_capability_control_kind_t kind = 0U;
    bool link_ready = false;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || rx_plan == NULL ||
        now_us == 0U || workspace == NULL || result_out == NULL ||
        rx_plan->struct_size != sizeof(*rx_plan) ||
        rx_plan->schema != UCN_I_COMPOSITION_CONTROL_SCHEMA ||
        rx_plan->runtime_instance != runtime->runtime_instance ||
        rx_plan->facts.now_us != now_us ||
        rx_plan->link_instance_generation == 0U ||
        rx_plan->facts.link_generation !=
            rx_plan->link_instance_generation ||
        rx_plan->has_hop_replay > 1U ||
        !composition_control_rx_hop_state_valid(rx_plan) ||
        rx_plan->reserved_zero != 0U ||
        !composition_external_range(runtime, rx_plan, sizeof(*rx_plan)) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        !composition_external_range(runtime, result_out,
                                    sizeof(*result_out)) ||
        (context != NULL &&
         !composition_external_range(runtime, context, sizeof(*context))) ||
        ucn_i_ranges_overlap(rx_plan, sizeof(*rx_plan), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(rx_plan, sizeof(*rx_plan), result_out,
                             sizeof(*result_out)) ||
        (context != NULL &&
         (ucn_i_ranges_overlap(rx_plan, sizeof(*rx_plan), context,
                               sizeof(*context)) ||
          ucn_i_ranges_overlap(context, sizeof(*context), workspace,
                               sizeof(*workspace)) ||
          ucn_i_ranges_overlap(context, sizeof(*context), result_out,
                               sizeof(*result_out)))) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), result_out,
                             sizeof(*result_out)) ||
        runtime->foundation_node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    plan = &rx_plan->dispatch;
    authenticated = &plan->authenticated;
    if (plan->struct_size != sizeof(*plan) ||
        plan->schema != UCN_I_COMPOSITION_CONTROL_SCHEMA ||
        plan->runtime_instance != runtime->runtime_instance ||
        plan->target_module != UCN_I_COMPOSITION_MODULE_CAPABILITY ||
        plan->opcode_domain != UCN_I_OPCODE_CAPABILITY_NEIGHBOR ||
        (authenticated->body == NULL && authenticated->body_bytes != 0U) ||
        !capability_control_contract(authenticated, &expected_body_bytes,
                                     &kind) ||
        !capability_control_context_valid(context, kind) ||
        !composition_external_range(runtime, authenticated->body,
                                    expected_body_bytes) ||
        ucn_i_ranges_overlap(authenticated->body,
                             expected_body_bytes, rx_plan,
                             sizeof(*rx_plan)) ||
        ucn_i_ranges_overlap(authenticated->body,
                             expected_body_bytes, workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(authenticated->body,
                             expected_body_bytes, result_out,
                             sizeof(*result_out)) ||
        (context != NULL &&
         ucn_i_ranges_overlap(authenticated->body, expected_body_bytes,
                              context, sizeof(*context)))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_control_enter(
        runtime, UCN_I_COMPOSITION_MODULE_CAPABILITY,
        UCN_I_COMPOSITION_OP_CAPABILITY_CONTROL, &claim);
    if (result != UCN_OK) {
        return result;
    }
    security_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    capability_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_CAPABILITY);
    capability_record = owner_record_find(
        runtime, UCN_I_COMPOSITION_MODULE_CAPABILITY);
    if (security_owner == NULL || capability_owner == NULL ||
        capability_record == NULL ||
        !owner_ref_equal(&plan->target, &capability_record->ref) ||
        authenticated->runtime_instance != runtime->runtime_instance ||
        authenticated->realm_id != capability_owner->realm_id) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    security_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    if (security_owner == NULL) {
        return UCN_ERR_STATE;
    }
    result = ucn_i_adapter_link_snapshot(
        &runtime->foundation_node->adapter, rx_plan->link_slot,
        &current_link_instance, &current_link_mtu, &link_ready);
    if (result != UCN_OK || !link_ready ||
        current_link_instance != rx_plan->link_instance_generation) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_security_control_fact_validate(
        security_owner, authenticated, &rx_plan->facts,
        &workspace->validate);
    if (result == UCN_OK) {
        result = ucn_i_security_session_get(
            security_owner, authenticated->session, &workspace->session);
    }
    if (result != UCN_OK ||
        !security_session_matches_control_rx(
            &workspace->session, rx_plan, now_us)) {
        return composition_leave(
            runtime, &claim, result == UCN_OK ? UCN_ERR_STATE : result);
    }
    capability_peer_view_build(runtime, rx_plan, &workspace->session,
                               &workspace->peer);
    memset(&workspace->candidate, 0, sizeof(workspace->candidate));
    workspace->candidate.struct_size =
        (uint16_t)sizeof(workspace->candidate);
    workspace->candidate.schema =
        UCN_I_COMPOSITION_CAPABILITY_CONTROL_SCHEMA;
    workspace->candidate.runtime_instance = runtime->runtime_instance;
    workspace->candidate.transaction_id = authenticated->transaction_id;
    workspace->candidate.opcode = authenticated->protocol_opcode;
    workspace->candidate.kind = kind;
    capability_ref_from_peer(&workspace->peer,
                             &workspace->candidate.reference);
    switch (kind) {
        case UCN_I_COMPOSITION_CAPABILITY_CONTROL_NEIGHBOR_HELLO:
        case UCN_I_COMPOSITION_CAPABILITY_CONTROL_NEIGHBOR_KEEPALIVE:
            result = ucn_i_capability_summary_decode(
                authenticated->body, &workspace->summary);
            if (result == UCN_OK) {
                result = ucn_i_capability_summary_ingest(
                    capability_owner, &workspace->peer,
                    &workspace->summary, now_us,
                    &workspace->candidate.summary_result);
            }
            break;
        case UCN_I_COMPOSITION_CAPABILITY_CONTROL_ADVERTISE:
            result = ucn_i_capability_record_decode(
                authenticated->body, &workspace->record);
            if (result == UCN_OK) {
                result = ucn_i_capability_advertise_ingest(
                    capability_owner, &workspace->peer, &workspace->record,
                    now_us, &workspace->candidate.reference);
            }
            break;
        case UCN_I_COMPOSITION_CAPABILITY_CONTROL_SELECT:
            result = ucn_i_profile_select_decode(
                authenticated->body, &workspace->selected);
            if (result == UCN_OK) {
                result = ucn_i_capability_get(
                    capability_owner, &workspace->candidate.reference,
                    now_us, &workspace->cached);
            }
            if (result == UCN_OK) {
                result = ucn_i_capability_digest(
                    &context->local_record, workspace->local_digest);
            }
            if (result == UCN_OK) {
                result = ucn_i_profile_select_build(
                    &context->local_record, workspace->local_digest,
                    &workspace->cached.record, workspace->cached.digest,
                    &context->requirements,
                    &workspace->candidate.selected);
            }
            if (result == UCN_OK) {
                uint32_t generation = workspace->candidate.selected.local_generation;
                uint8_t digest[16];
                workspace->candidate.selected.local_generation = workspace->candidate.selected.peer_generation;
                workspace->candidate.selected.peer_generation = generation;
                memcpy(digest,workspace->candidate.selected.local_digest,16U);
                memcpy(workspace->candidate.selected.local_digest,workspace->candidate.selected.peer_digest,16U);
                memcpy(workspace->candidate.selected.peer_digest,digest,16U);
            }
            if (result == UCN_OK &&
                memcmp(&workspace->selected,
                       &workspace->candidate.selected,
                       sizeof(workspace->selected)) != 0) {
                result = UCN_ERR_SECURITY;
            }
            break;
        case UCN_I_COMPOSITION_CAPABILITY_CONTROL_ACK:
            result = ucn_i_profile_ack_decode(
                authenticated->body, &workspace->acknowledged);
            if (result == UCN_OK) {
                result = ucn_i_profile_ack_verify_exact(
                    &workspace->acknowledged, &context->expected_select,
                    context->expected_transcript_digest);
            }
            if (result == UCN_OK) {
                workspace->candidate.acknowledged =
                    workspace->acknowledged;
            }
            break;
        case UCN_I_COMPOSITION_CAPABILITY_CONTROL_REVOKE:
            result = ucn_i_capability_query_decode(
                authenticated->body, &workspace->query);
            if (result == UCN_OK) {
                result = ucn_i_capability_get(
                    capability_owner, &workspace->candidate.reference,
                    now_us, &workspace->cached);
            }
            if (result == UCN_OK &&
                (workspace->query.requested_generation !=
                     workspace->cached.record.capability_generation ||
                 memcmp(workspace->query.known_digest,
                        workspace->cached.digest,
                        sizeof(workspace->query.known_digest)) != 0)) {
                result = UCN_ERR_SECURITY;
            }
            if (result == UCN_OK) {
                result = ucn_i_capability_revoke(
                    capability_owner, &workspace->candidate.reference,
                    &workspace->query);
            }
            if (result == UCN_OK) {
                workspace->candidate.revoked = workspace->query;
            }
            break;
        case UCN_I_COMPOSITION_CAPABILITY_CONTROL_QUERY:
            result = ucn_i_capability_query_decode(authenticated->body, &workspace->candidate.query);
            break;
        default:
            result = UCN_ERR_UNSUPPORTED;
            break;
    }
    if (result == UCN_OK) {
        result = ucn_i_security_control_replay_complete(
            security_owner, &(authenticated->replay), &rx_plan->facts);
        if (result != UCN_OK) {
            composition_mark_fault(runtime);
            result = UCN_ERR_IN_DOUBT;
        }
    }
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *result_out = workspace->candidate;
    }
    (void)current_link_mtu;
    return result;
}

static bool composition_bytes_nonzero(const uint8_t *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool transport_control_contract(
    const ucn_i_security_authenticated_control_t *authenticated,
    size_t *body_bytes_out,
    ucn_i_composition_transport_control_kind_t *kind_out)
{
    size_t body_bytes;
    ucn_i_composition_transport_control_kind_t kind;
    uint8_t role;

    if (authenticated == NULL || body_bytes_out == NULL || kind_out == NULL ||
        authenticated->traffic_class != UCN_TRAFFIC_Q1 ||
        authenticated->delivery_guarantee != UCN_DELIVERY_BEST_EFFORT ||
        authenticated->payload_kind != UCN_I_PAYLOAD_CONTROL ||
        authenticated->origin_security == UCN_I_ORIGIN_O0) {
        return false;
    }
    switch (authenticated->protocol_opcode) {
        case UCN_I_OPCODE_TRANSFER_SETUP:
            body_bytes = authenticated->body_bytes;
            if (body_bytes != UCN_I_TRANSPORT_TRANSFER_SETUP_ONE_WAY_BYTES &&
                body_bytes !=
                    UCN_I_TRANSPORT_TRANSFER_SETUP_OPERATION_BYTES &&
                body_bytes != UCN_I_TRANSPORT_TRANSFER_SETUP_RESULT_BYTES) {
                return false;
            }
            role = UCN_INTERACTION_REQUEST;
            kind = UCN_I_COMPOSITION_TRANSPORT_CONTROL_SETUP;
            break;
        case UCN_I_OPCODE_TRANSFER_SETUP_ACK:
            body_bytes = UCN_I_TRANSPORT_TRANSFER_SETUP_ACK_BYTES;
            role = UCN_INTERACTION_RESULT;
            kind = UCN_I_COMPOSITION_TRANSPORT_CONTROL_SETUP_ACK;
            break;
        case UCN_I_OPCODE_TRANSFER_ABORT:
            body_bytes = UCN_I_TRANSPORT_TRANSFER_ABORT_BYTES;
            role = UCN_INTERACTION_ERROR;
            kind = UCN_I_COMPOSITION_TRANSPORT_CONTROL_ABORT;
            break;
        case UCN_I_OPCODE_TRANSFER_TERMINAL_RECEIPT:
            body_bytes = UCN_I_TRANSPORT_TRANSFER_TERMINAL_BYTES;
            role = UCN_INTERACTION_RESULT;
            kind = UCN_I_COMPOSITION_TRANSPORT_CONTROL_TERMINAL;
            break;
        default:
            return false;
    }
    if (authenticated->interaction_role != role ||
        authenticated->body_bytes != body_bytes) {
        return false;
    }
    *body_bytes_out = body_bytes;
    *kind_out = kind;
    return true;
}

static void transport_binding_from_security(
    const ucn_i_security_binding_t *source,
    ucn_i_transport_binding_t *destination)
{
    memset(destination, 0, sizeof(*destination));
    destination->address = source->address;
    destination->generation = source->binding_generation;
    memcpy(destination->principal, source->principal,
           sizeof(destination->principal));
}

static void transport_facts_from_control(
    const ucn_i_composition_control_rx_plan_t *rx_plan,
    const ucn_i_security_session_view_t *session,
    const ucn_i_composition_transport_control_context_t *context,
    uint8_t parent_kind,
    ucn_i_transport_transfer_facts_t *facts)
{
    memset(facts, 0, sizeof(*facts));
    transport_binding_from_security(&rx_plan->facts.peer, &facts->source);
    transport_binding_from_security(&rx_plan->facts.local,
                                    &facts->destination);
    facts->security.session_generation = session->session_generation;
    facts->security.key_generation = session->origin_rx_key_generation;
    facts->security.origin_security =
        rx_plan->dispatch.authenticated.origin_security;
    facts->realm = rx_plan->dispatch.authenticated.realm_id;
    facts->transport_policy_generation =
        context->transport_policy_generation;
    facts->parent_kind = parent_kind;
    memcpy(facts->parent_fingerprint, context->parent_fingerprint,
           sizeof(facts->parent_fingerprint));
}

static bool transport_key_equal(
    const ucn_i_transport_transfer_key_t *left,
    const ucn_i_transport_transfer_key_t *right)
{
    return left->parent_kind == right->parent_kind &&
           left->parent_id == right->parent_id &&
           left->parent_generation == right->parent_generation &&
           left->transfer_id == right->transfer_id &&
           left->service_id == right->service_id &&
           memcmp(left->setup_digest, right->setup_digest,
                  sizeof(left->setup_digest)) == 0;
}

ucn_result_t ucn_i_composition_transport_control_handle(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_rx_plan_t *rx_plan,
    uint64_t now_us,
    const ucn_i_composition_transport_control_context_t *context,
    ucn_i_composition_transport_control_workspace_t *workspace,
    ucn_i_composition_transport_control_result_t *result_out)
{
    const ucn_i_composition_control_dispatch_plan_t *plan;
    const ucn_i_security_authenticated_control_t *authenticated;
    const ucn_i_composition_owner_record_t *transport_record;
    ucn_i_security_owner_t *security_owner;
    ucn_i_transport_owner_t *transport_owner;
    ucn_i_callback_claim_t claim;
    size_t expected_body_bytes = 0U;
    ucn_i_composition_transport_control_kind_t kind = 0U;
    bool duplicate = false;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || rx_plan == NULL ||
        now_us == 0U || context == NULL || workspace == NULL ||
        result_out == NULL || rx_plan->struct_size != sizeof(*rx_plan) ||
        rx_plan->schema != UCN_I_COMPOSITION_CONTROL_SCHEMA ||
        rx_plan->runtime_instance != runtime->runtime_instance ||
        rx_plan->facts.now_us != now_us ||
        !composition_control_rx_hop_state_valid(rx_plan) ||
        context->struct_size != sizeof(*context) ||
        context->schema != UCN_I_COMPOSITION_TRANSPORT_CONTROL_SCHEMA ||
        context->transport_policy_generation == 0U ||
        context->parent_valid > 1U || context->transfer_valid > 1U ||
        !composition_external_range(runtime, rx_plan, sizeof(*rx_plan)) ||
        !composition_external_range(runtime, context, sizeof(*context)) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        !composition_external_range(runtime, result_out,
                                    sizeof(*result_out)) ||
        ucn_i_ranges_overlap(rx_plan, sizeof(*rx_plan), context,
                             sizeof(*context)) ||
        ucn_i_ranges_overlap(rx_plan, sizeof(*rx_plan), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(rx_plan, sizeof(*rx_plan), result_out,
                             sizeof(*result_out)) ||
        ucn_i_ranges_overlap(context, sizeof(*context), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(context, sizeof(*context), result_out,
                             sizeof(*result_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), result_out,
                             sizeof(*result_out))) {
        return UCN_ERR_ARGUMENT;
    }
    plan = &rx_plan->dispatch;
    authenticated = &plan->authenticated;
    if (plan->target_module != UCN_I_COMPOSITION_MODULE_TRANSPORT ||
        plan->opcode_domain != UCN_I_OPCODE_TRANSPORT ||
        !transport_control_contract(authenticated, &expected_body_bytes,
                                    &kind) ||
        !composition_external_range(runtime, authenticated->body,
                                    expected_body_bytes) ||
        ucn_i_ranges_overlap(authenticated->body, expected_body_bytes,
                             context, sizeof(*context)) ||
        ucn_i_ranges_overlap(authenticated->body, expected_body_bytes,
                             workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(authenticated->body, expected_body_bytes,
                             result_out, sizeof(*result_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_control_enter(
        runtime, UCN_I_COMPOSITION_MODULE_TRANSPORT,
        UCN_I_COMPOSITION_OP_TRANSPORT_CONTROL, &claim);
    if (result != UCN_OK) {
        return result;
    }
    security_owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    transport_owner = owner_storage(runtime,
                                    UCN_I_COMPOSITION_MODULE_TRANSPORT);
    transport_record = owner_record_find(
        runtime, UCN_I_COMPOSITION_MODULE_TRANSPORT);
    if (security_owner == NULL || transport_owner == NULL ||
        transport_record == NULL ||
        !owner_ref_equal(&plan->target, &transport_record->ref)) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_security_control_fact_validate(
        security_owner, authenticated, &rx_plan->facts,
        &workspace->validate);
    if (result == UCN_OK) {
        result = ucn_i_security_session_get(
            security_owner, authenticated->session, &workspace->session);
    }
    if (result != UCN_OK ||
        !security_session_matches_control_rx(
            &workspace->session, rx_plan, now_us) ||
        workspace->session.origin_rx_key_generation == 0U) {
        return composition_leave(
            runtime, &claim, result == UCN_OK ? UCN_ERR_STATE : result);
    }
    memset(&workspace->candidate, 0, sizeof(workspace->candidate));
    workspace->candidate.struct_size =
        (uint16_t)sizeof(workspace->candidate);
    workspace->candidate.schema =
        UCN_I_COMPOSITION_TRANSPORT_CONTROL_SCHEMA;
    workspace->candidate.runtime_instance = runtime->runtime_instance;
    workspace->candidate.transaction_id = authenticated->transaction_id;
    workspace->candidate.opcode = authenticated->protocol_opcode;
    workspace->candidate.kind = kind;

    if (kind == UCN_I_COMPOSITION_TRANSPORT_CONTROL_SETUP) {
        result = ucn_i_transport_transfer_setup_decode(
            authenticated->body, authenticated->body_bytes,
            &workspace->setup);
        if (result == UCN_OK) {
            bool fingerprint_present = composition_bytes_nonzero(
                context->parent_fingerprint,
                sizeof(context->parent_fingerprint));
            if ((workspace->setup.parent_kind ==
                     UCN_I_TRANSPORT_PARENT_KIND_C1 &&
                 (context->parent_valid == 0U || fingerprint_present)) ||
                (workspace->setup.parent_kind ==
                     UCN_I_TRANSPORT_PARENT_KIND_FLOW &&
                 (context->parent_valid != 0U || !fingerprint_present))) {
                result = UCN_ERR_STATE;
            }
        }
        if (result == UCN_OK) {
            transport_facts_from_control(
                rx_plan, &workspace->session, context,
                workspace->setup.parent_kind, &workspace->facts);
            if (workspace->setup.parent_kind ==
                UCN_I_TRANSPORT_PARENT_KIND_C1) {
                result = ucn_i_transport_c1_transfer_rx_setup(
                    transport_owner, context->parent, &workspace->setup,
                    &workspace->facts, now_us,
                    &workspace->candidate.transfer, &duplicate);
            } else {
                result = ucn_i_transport_transfer_rx_setup(
                    transport_owner, &workspace->setup, &workspace->facts,
                    now_us, &workspace->candidate.transfer, &duplicate);
            }
        }
        if (result == UCN_OK) {
            result = ucn_i_transport_transfer_key_from_setup(
                &workspace->setup, &workspace->candidate.setup_ack.key);
        }
        if (result == UCN_OK) {
            workspace->candidate.setup_ack.accepted_fragment_budget =
                workspace->setup.fragment_budget;
            workspace->candidate.setup_ack.receive_credit =
                context->receive_credit;
            workspace->candidate.exact_duplicate = duplicate ? 1U : 0U;
        }
    } else {
        if (context->transfer_valid == 0U) {
            result = UCN_ERR_STATE;
        } else {
            result = ucn_i_transport_transfer_view(
                transport_owner, context->transfer, &workspace->view);
        }
        if (result == UCN_OK) {
            result = ucn_i_transport_transfer_key_from_setup(
                &workspace->view.setup, &workspace->expected_key);
        }
        if (result == UCN_OK) {
            transport_facts_from_control(
                rx_plan, &workspace->session, context,
                workspace->view.setup.parent_kind, &workspace->facts);
        }
        if (result == UCN_OK &&
            kind == UCN_I_COMPOSITION_TRANSPORT_CONTROL_SETUP_ACK) {
            result = ucn_i_transport_transfer_setup_ack_decode(
                authenticated->body, &workspace->setup_ack);
            if (result == UCN_OK &&
                (!transport_key_equal(&workspace->expected_key,
                                      &workspace->setup_ack.key) ||
                 workspace->setup_ack.accepted_fragment_budget !=
                     workspace->view.setup.fragment_budget)) {
                result = UCN_ERR_STATE;
            }
            if (result == UCN_OK) {
                result = ucn_i_transport_transfer_tx_accept_setup(
                    transport_owner, context->transfer,
                    &workspace->view.setup, &workspace->facts, now_us);
            }
        } else if (result == UCN_OK &&
                   kind == UCN_I_COMPOSITION_TRANSPORT_CONTROL_ABORT) {
            result = ucn_i_transport_transfer_abort_body_decode(
                authenticated->body, &workspace->abort_body);
            if (result == UCN_OK &&
                !transport_key_equal(&workspace->expected_key,
                                     &workspace->abort_body.key)) {
                result = UCN_ERR_STATE;
            }
            if (result == UCN_OK) {
                result = ucn_i_transport_transfer_accept_abort(
                    transport_owner, context->transfer,
                    &workspace->abort_body.key, &workspace->facts, now_us);
            }
        } else if (result == UCN_OK &&
                   kind == UCN_I_COMPOSITION_TRANSPORT_CONTROL_TERMINAL) {
            result = ucn_i_transport_transfer_terminal_body_decode(
                authenticated->body, &workspace->terminal_body);
            if (result == UCN_OK &&
                !transport_key_equal(&workspace->expected_key,
                                     &workspace->terminal_body.key)) {
                result = UCN_ERR_STATE;
            }
            if (result == UCN_OK) {
                result = ucn_i_transport_transfer_tx_accept_terminal(
                    transport_owner, context->transfer,
                    &workspace->terminal_body.key, &workspace->facts,
                    workspace->terminal_body.terminal_state,
                    workspace->terminal_body.result_code, now_us);
            }
        }
        if (result == UCN_OK) {
            workspace->candidate.transfer = context->transfer;
        }
    }
    if (result == UCN_OK) {
        result = ucn_i_security_control_replay_complete(
            security_owner, &(authenticated->replay), &rx_plan->facts);
        if (result != UCN_OK) {
            composition_mark_fault(runtime);
            result = UCN_ERR_IN_DOUBT;
        }
    }
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *result_out = workspace->candidate;
    }
    return result;
}

static bool service_control_contract(
    const ucn_i_security_authenticated_control_t *authenticated,
    size_t *body_bytes_out,
    ucn_i_composition_service_control_kind_t *kind_out)
{
    size_t body_bytes;
    ucn_i_composition_service_control_kind_t kind;
    uint8_t role;

    if (authenticated == NULL || body_bytes_out == NULL || kind_out == NULL ||
        authenticated->traffic_class != UCN_TRAFFIC_Q1 ||
        authenticated->delivery_guarantee != UCN_DELIVERY_RELIABLE ||
        authenticated->payload_kind != UCN_I_PAYLOAD_CONTROL ||
        authenticated->origin_security == UCN_I_ORIGIN_O0) {
        return false;
    }
    switch (authenticated->protocol_opcode) {
        case UCN_I_OPCODE_OPERATION_QUERY:
            body_bytes = UCN_I_SERVICE_OPERATION_QUERY_BYTES;
            kind = UCN_I_COMPOSITION_SERVICE_CONTROL_QUERY;
            role = UCN_INTERACTION_REQUEST;
            break;
        case UCN_I_OPCODE_OPERATION_RECEIPT:
            body_bytes = UCN_I_SERVICE_OPERATION_RECEIPT_BYTES;
            kind = UCN_I_COMPOSITION_SERVICE_CONTROL_RECEIPT;
            role = UCN_INTERACTION_RESULT;
            break;
        case UCN_I_OPCODE_OPERATION_CANCEL:
            body_bytes = UCN_I_SERVICE_OPERATION_CANCEL_BYTES;
            kind = UCN_I_COMPOSITION_SERVICE_CONTROL_CANCEL;
            role = UCN_INTERACTION_REQUEST;
            break;
        default:
            return false;
    }
    if (authenticated->interaction_role != role ||
        authenticated->body_bytes != body_bytes) {
        return false;
    }
    *body_bytes_out = body_bytes;
    *kind_out = kind;
    return true;
}

static bool service_binding_matches_security(
    const ucn_i_service_binding_t *service,
    const ucn_i_security_binding_t *security)
{
    return service->address == security->address &&
           service->generation == security->binding_generation &&
           memcmp(service->principal, security->principal,
                  sizeof(service->principal)) == 0;
}

static bool service_wire_key_matches_view(
    const ucn_i_service_operation_wire_key_t *wire,
    const ucn_i_service_operation_view_t *view)
{
    return wire->operation_id == view->key.service.operation_id &&
           wire->service_id == view->key.service.service_id &&
           wire->service_opcode == view->key.service.opcode &&
           memcmp(wire->request_digest, view->key.request_digest,
                  sizeof(wire->request_digest)) == 0;
}

static ucn_result_t service_result_digest(
    const ucn_i_service_operation_view_t *view,
    ucn_i_composition_service_control_workspace_t *workspace)
{
    uint32_t result_bits;
    uint16_t bytes;

    if (view->result.bytes > UCN_I_SERVICE_RESULT_BYTES) {
        return UCN_ERR_STATE;
    }
    bytes = view->result.bytes;
    result_bits = (uint32_t)view->result.application_result;
    memset(workspace->result_canonical, 0,
           sizeof(workspace->result_canonical));
    workspace->result_canonical[0] = view->phase;
    workspace->result_canonical[1] = (uint8_t)(result_bits >> 24U);
    workspace->result_canonical[2] = (uint8_t)(result_bits >> 16U);
    workspace->result_canonical[3] = (uint8_t)(result_bits >> 8U);
    workspace->result_canonical[4] = (uint8_t)result_bits;
    workspace->result_canonical[5] = (uint8_t)(bytes >> 8U);
    workspace->result_canonical[6] = (uint8_t)bytes;
    if (bytes != 0U) {
        memcpy(workspace->result_canonical + 7U,
               view->result.payload, bytes);
    }
    return ucn_i_sha256_128(
        workspace->result_canonical, 7U + bytes,
        workspace->result_digest, &workspace->digest_workspace);
}

ucn_result_t ucn_i_composition_service_control_handle(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_rx_plan_t *rx_plan,
    uint64_t now_us,
    const ucn_i_composition_service_control_context_t *context,
    ucn_i_composition_service_control_workspace_t *workspace,
    ucn_i_composition_service_control_result_t *result_out)
{
    const ucn_i_composition_control_dispatch_plan_t *plan;
    const ucn_i_security_authenticated_control_t *authenticated;
    const ucn_i_composition_owner_record_t *service_record;
    ucn_i_security_owner_t *security_owner;
    ucn_i_service_owner_t *service_owner;
    ucn_i_callback_claim_t claim;
    size_t expected_body_bytes = 0U;
    ucn_i_composition_service_control_kind_t kind = 0U;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || rx_plan == NULL ||
        context == NULL || workspace == NULL || result_out == NULL ||
        now_us == 0U || context->struct_size != sizeof(*context) ||
        context->schema != UCN_I_COMPOSITION_SERVICE_CONTROL_SCHEMA ||
        context->operation_valid != 1U || context->durability_valid > 1U ||
        !bytes_are_zero(context->reserved_zero,
                        sizeof(context->reserved_zero)) ||
        !composition_external_range(runtime, rx_plan, sizeof(*rx_plan)) ||
        !composition_external_range(runtime, context, sizeof(*context)) ||
        !composition_external_range(runtime, workspace,
                                    sizeof(*workspace)) ||
        !composition_external_range(runtime, result_out,
                                    sizeof(*result_out)) ||
        ucn_i_ranges_overlap(rx_plan, sizeof(*rx_plan), context,
                             sizeof(*context)) ||
        ucn_i_ranges_overlap(rx_plan, sizeof(*rx_plan), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(rx_plan, sizeof(*rx_plan), result_out,
                             sizeof(*result_out)) ||
        ucn_i_ranges_overlap(context, sizeof(*context), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(context, sizeof(*context), result_out,
                             sizeof(*result_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), result_out,
                             sizeof(*result_out))) {
        return UCN_ERR_ARGUMENT;
    }
    plan = &rx_plan->dispatch;
    authenticated = &plan->authenticated;
    if (plan->target_module != UCN_I_COMPOSITION_MODULE_SERVICE ||
        plan->opcode_domain != UCN_I_OPCODE_SERVICE_OPERATION ||
        !service_control_contract(authenticated, &expected_body_bytes,
                                  &kind) ||
        !composition_external_range(runtime, authenticated->body,
                                    expected_body_bytes) ||
        ucn_i_ranges_overlap(authenticated->body, expected_body_bytes,
                             context, sizeof(*context)) ||
        ucn_i_ranges_overlap(authenticated->body, expected_body_bytes,
                             workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(authenticated->body, expected_body_bytes,
                             result_out, sizeof(*result_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_control_enter(
        runtime, UCN_I_COMPOSITION_MODULE_SERVICE,
        UCN_I_COMPOSITION_OP_SERVICE_CONTROL, &claim);
    if (result != UCN_OK) {
        return result;
    }
    security_owner = owner_storage(runtime,
                                   UCN_I_COMPOSITION_MODULE_SECURITY);
    service_owner = owner_storage(runtime,
                                  UCN_I_COMPOSITION_MODULE_SERVICE);
    service_record = owner_record_find(runtime,
                                       UCN_I_COMPOSITION_MODULE_SERVICE);
    if (security_owner == NULL || service_owner == NULL ||
        service_record == NULL ||
        !owner_ref_equal(&plan->target, &service_record->ref)) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_security_control_fact_validate(
        security_owner, authenticated, &rx_plan->facts,
        &workspace->validate);
    if (result == UCN_OK) {
        result = ucn_i_security_session_get(
            security_owner, authenticated->session, &workspace->session);
    }
    if (result != UCN_OK ||
        !security_session_matches_control_rx(&workspace->session, rx_plan,
                                             now_us) ||
        workspace->session.origin_rx_key_generation == 0U) {
        return composition_leave(
            runtime, &claim, result == UCN_OK ? UCN_ERR_STATE : result);
    }
    result = ucn_i_service_operation_view(
        service_owner, context->operation, &workspace->view);
    if (result == UCN_OK &&
        (!service_binding_matches_security(
             &workspace->view.key.service.client, &rx_plan->facts.peer) ||
         !service_binding_matches_security(
             &workspace->view.key.service.server, &rx_plan->facts.local) ||
         workspace->view.key.service.realm != authenticated->realm_id ||
         workspace->view.key.service.security.session_generation !=
             workspace->session.session_generation ||
         workspace->view.key.service.security.key_generation !=
             workspace->session.origin_rx_key_generation ||
         workspace->view.key.service.security.policy_generation !=
             workspace->session.policy_generation ||
         workspace->view.key.service.security.origin_security !=
             authenticated->origin_security ||
         workspace->view.key.service.security.acl_authorized != 1U)) {
        result = UCN_ERR_STATE;
    }
    memset(&workspace->candidate, 0, sizeof(workspace->candidate));
    workspace->candidate.struct_size =
        (uint16_t)sizeof(workspace->candidate);
    workspace->candidate.schema = UCN_I_COMPOSITION_SERVICE_CONTROL_SCHEMA;
    workspace->candidate.runtime_instance = runtime->runtime_instance;
    workspace->candidate.transaction_id = authenticated->transaction_id;
    workspace->candidate.opcode = authenticated->protocol_opcode;
    workspace->candidate.kind = kind;

    if (result == UCN_OK &&
        kind == UCN_I_COMPOSITION_SERVICE_CONTROL_QUERY) {
        result = ucn_i_service_operation_wire_key_decode(
            authenticated->body, &workspace->key);
        if (result == UCN_OK &&
            !service_wire_key_matches_view(&workspace->key,
                                           &workspace->view)) {
            result = UCN_ERR_STATE;
        }
    } else if (result == UCN_OK &&
               kind == UCN_I_COMPOSITION_SERVICE_CONTROL_RECEIPT) {
        result = ucn_i_service_operation_receipt_decode(
            authenticated->body, &workspace->receipt);
        if (result == UCN_OK &&
            (!service_wire_key_matches_view(&workspace->receipt.key,
                                            &workspace->view) ||
             workspace->receipt.terminal_phase != workspace->view.phase)) {
            result = UCN_ERR_STATE;
        }
        if (result == UCN_OK) {
            result = service_result_digest(&workspace->view, workspace);
        }
        if (result == UCN_OK &&
            memcmp(workspace->receipt.result_digest,
                   workspace->result_digest,
                   sizeof(workspace->result_digest)) != 0) {
            result = UCN_ERR_SECURITY;
        }
    } else if (result == UCN_OK &&
               kind == UCN_I_COMPOSITION_SERVICE_CONTROL_CANCEL) {
        result = ucn_i_service_operation_cancel_decode(
            authenticated->body, &workspace->cancel);
        if (result == UCN_OK &&
            !service_wire_key_matches_view(&workspace->cancel.key,
                                           &workspace->view)) {
            result = UCN_ERR_STATE;
        }
        if (result == UCN_OK && context->durability_valid != 1U) {
            result = UCN_ERR_STATE;
        }
        if (result == UCN_OK) {
            result = ucn_i_service_operation_prepare_terminal(
                service_owner, context->operation,
                UCN_I_SERVICE_OPERATION_ABORTED_NO_EFFECT, NULL,
                &context->durability);
        }
        if (result == UCN_OK) {
            result = ucn_i_service_operation_requirement_get(
                service_owner, context->operation,
                &workspace->candidate.requirement);
        }
        if (result == UCN_OK) {
            workspace->candidate.persistence_required = 1U;
            result = ucn_i_service_operation_view(
                service_owner, context->operation, &workspace->view);
        }
    }
    if (result == UCN_OK) {
        workspace->candidate.view = workspace->view;
        result = ucn_i_security_control_replay_complete(
            security_owner, &(authenticated->replay), &rx_plan->facts);
        if (result != UCN_OK) {
            composition_mark_fault(runtime);
            result = UCN_ERR_IN_DOUBT;
        }
    }
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *result_out = workspace->candidate;
    }
    return result;
}

/* EN: A generic Control submitter is only a transport boundary.  It may
 * carry a body whose exact typed contract is already implemented, but it
 * must never become an escape hatch for a registered-only opcode or for
 * arbitrary bytes that merely pass crypto validation.
 * 中文：通用 Control 提交入口只是传输边界。它只能承载已经实现精确强类型
 * 合同的 Body，绝不能成为“仅登记 Opcode”或任意字节绕过 typed encoder 的
 * 后门。 */
typedef union composition_control_tx_validation {
    ucn_i_security_authenticated_control_t typed;
    ucn_i_route_rreq_message_t route_rreq;
    ucn_i_route_rrep_message_t route_rrep;
    ucn_i_route_rerr_payload_t route_rerr;
    ucn_i_capability_summary_t capability_summary;
    ucn_i_capability_record_t capability_record;
    ucn_i_profile_select_t profile_select;
    ucn_i_profile_ack_t profile_ack;
    ucn_i_capability_query_t capability_query;
    ucn_i_diagnostic_echo_t diagnostic;
    ucn_i_transport_transfer_setup_t transfer_setup;
    ucn_i_transport_transfer_setup_ack_t transfer_setup_ack;
    ucn_i_transport_transfer_abort_body_t transfer_abort;
    ucn_i_transport_transfer_terminal_body_t transfer_terminal;
    ucn_i_service_operation_wire_key_t operation_query;
    ucn_i_service_operation_receipt_body_t operation_receipt;
    ucn_i_service_operation_cancel_body_t operation_cancel;
} composition_control_tx_validation_t;

static ucn_result_t composition_control_tx_body_validate(
    const ucn_i_security_c0_protect_request_t *request,
    const uint8_t *body,
    size_t body_bytes)
{
    composition_control_tx_validation_t scratch;
    ucn_i_protocol_opcode_support_t support;
    size_t expected_body_bytes = 0U;
    ucn_i_composition_capability_control_kind_t capability_kind = 0U;
    ucn_i_composition_transport_control_kind_t transport_kind = 0U;
    ucn_i_composition_service_control_kind_t service_kind = 0U;
    uint8_t expected_role;
    uint32_t reporter_generation;
    ucn_result_t result;

    result = ucn_i_protocol_opcode_support(request->protocol_opcode,
                                            &support);
    if (result != UCN_OK) {
        return result;
    }
    if ((support & UCN_I_OPCODE_SUPPORT_AUTH_TERMINAL) == 0U) {
        return UCN_ERR_UNSUPPORTED;
    }
    memset(&scratch, 0, sizeof(scratch));
    scratch.typed.body = body;
    scratch.typed.body_bytes = body_bytes;
    scratch.typed.protocol_opcode = request->protocol_opcode;
    scratch.typed.traffic_class = request->traffic_class;
    scratch.typed.delivery_guarantee = request->delivery_guarantee;
    scratch.typed.interaction_role = request->interaction_role;
    scratch.typed.payload_kind = request->payload_kind;
    /* This entry is authenticated-only.  Security validates the exact O1/O2
     * selector; contract selection merely needs a non-bootstrap level. */
    scratch.typed.origin_security = UCN_I_ORIGIN_O1;
    scratch.typed.hop_limit = request->hop_limit;

    switch (request->protocol_opcode) {
        case UCN_I_OPCODE_ROUTE_RREQ:
            if (!route_control_semantics_valid(&scratch.typed) ||
                body_bytes != UCN_I_ROUTE_CONTROL_REQUEST_BYTES) {
                return UCN_ERR_ARGUMENT;
            }
            return ucn_i_route_control_request_decode(
                body, body_bytes, 4U, &scratch.route_rreq);
        case UCN_I_OPCODE_ROUTE_RREP:
            if (!route_control_semantics_valid(&scratch.typed) ||
                body_bytes != UCN_I_ROUTE_CONTROL_REPLY_BYTES) {
                return UCN_ERR_ARGUMENT;
            }
            return ucn_i_route_control_reply_decode(
                body, body_bytes, 4U, &scratch.route_rrep,
                &reporter_generation);
        case UCN_I_OPCODE_ROUTE_RERR:
            if (!route_control_semantics_valid(&scratch.typed) ||
                body_bytes != UCN_I_ROUTE_RERR_BYTES) {
                return UCN_ERR_ARGUMENT;
            }
            return ucn_i_route_rerr_decode(body, &scratch.route_rerr);
        case UCN_I_OPCODE_NEIGHBOR_HELLO:
        case UCN_I_OPCODE_NEIGHBOR_KEEPALIVE:
        case UCN_I_OPCODE_CAPABILITY_ADVERTISE:
        case UCN_I_OPCODE_CAPABILITY_SELECT:
        case UCN_I_OPCODE_CAPABILITY_QUERY:
        case UCN_I_OPCODE_CAPABILITY_ACK:
        case UCN_I_OPCODE_CAPABILITY_REVOKE:
            if (!capability_control_contract(
                    &scratch.typed, &expected_body_bytes,
                    &capability_kind) || body_bytes != expected_body_bytes) {
                return UCN_ERR_ARGUMENT;
            }
            (void)capability_kind;
            if (request->protocol_opcode == UCN_I_OPCODE_NEIGHBOR_HELLO ||
                request->protocol_opcode ==
                    UCN_I_OPCODE_NEIGHBOR_KEEPALIVE) {
                return ucn_i_capability_summary_decode(
                    body, &scratch.capability_summary);
            }
            if (request->protocol_opcode ==
                UCN_I_OPCODE_CAPABILITY_ADVERTISE) {
                return ucn_i_capability_record_decode(
                    body, &scratch.capability_record);
            }
            if (request->protocol_opcode == UCN_I_OPCODE_CAPABILITY_SELECT) {
                return ucn_i_profile_select_decode(
                    body, &scratch.profile_select);
            }
            if (request->protocol_opcode == UCN_I_OPCODE_CAPABILITY_ACK) {
                return ucn_i_profile_ack_decode(body, &scratch.profile_ack);
            }
            return ucn_i_capability_query_decode(
                body, &scratch.capability_query);
        case UCN_I_OPCODE_DIAGNOSTIC_PING:
        case UCN_I_OPCODE_DIAGNOSTIC_PONG:
            expected_role = request->protocol_opcode ==
                                    UCN_I_OPCODE_DIAGNOSTIC_PING ?
                                UCN_INTERACTION_REQUEST :
                                UCN_INTERACTION_RESULT;
            if (request->traffic_class != UCN_TRAFFIC_Q0 ||
                request->delivery_guarantee !=
                    UCN_DELIVERY_BEST_EFFORT ||
                request->interaction_role != expected_role ||
                request->payload_kind != UCN_I_PAYLOAD_DIAGNOSTIC ||
                body_bytes != UCN_I_DIAGNOSTIC_ECHO_BYTES) {
                return UCN_ERR_ARGUMENT;
            }
            return ucn_i_diagnostic_echo_decode(body, &scratch.diagnostic);
        case UCN_I_OPCODE_TRANSFER_SETUP:
        case UCN_I_OPCODE_TRANSFER_SETUP_ACK:
        case UCN_I_OPCODE_TRANSFER_ABORT:
        case UCN_I_OPCODE_TRANSFER_TERMINAL_RECEIPT:
            if (!transport_control_contract(
                    &scratch.typed, &expected_body_bytes,
                    &transport_kind) ||
                body_bytes != expected_body_bytes) {
                return UCN_ERR_ARGUMENT;
            }
            (void)transport_kind;
            if (request->protocol_opcode == UCN_I_OPCODE_TRANSFER_SETUP) {
                return ucn_i_transport_transfer_setup_decode(
                    body, body_bytes, &scratch.transfer_setup);
            }
            if (request->protocol_opcode ==
                UCN_I_OPCODE_TRANSFER_SETUP_ACK) {
                return ucn_i_transport_transfer_setup_ack_decode(
                    body, &scratch.transfer_setup_ack);
            }
            if (request->protocol_opcode == UCN_I_OPCODE_TRANSFER_ABORT) {
                return ucn_i_transport_transfer_abort_body_decode(
                    body, &scratch.transfer_abort);
            }
            return ucn_i_transport_transfer_terminal_body_decode(
                body, &scratch.transfer_terminal);
        case UCN_I_OPCODE_OPERATION_QUERY:
        case UCN_I_OPCODE_OPERATION_RECEIPT:
        case UCN_I_OPCODE_OPERATION_CANCEL:
            if (!service_control_contract(
                    &scratch.typed, &expected_body_bytes,
                    &service_kind) ||
                body_bytes != expected_body_bytes) {
                return UCN_ERR_ARGUMENT;
            }
            (void)service_kind;
            if (request->protocol_opcode == UCN_I_OPCODE_OPERATION_QUERY) {
                return ucn_i_service_operation_wire_key_decode(
                    body, &scratch.operation_query);
            }
            if (request->protocol_opcode == UCN_I_OPCODE_OPERATION_RECEIPT) {
                return ucn_i_service_operation_receipt_decode(
                    body, &scratch.operation_receipt);
            }
            return ucn_i_service_operation_cancel_decode(
                body, &scratch.operation_cancel);
        default:
            return UCN_ERR_UNSUPPORTED;
    }
}

ucn_result_t ucn_i_composition_control_protect(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_security_c0_protect_request_t *request,
    const uint8_t *plaintext,
    size_t plaintext_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *packet_output,
    size_t packet_capacity,
    size_t *packet_bytes)
{
    ucn_i_security_owner_t *security_owner;
    ucn_i_composition_module_id_t target_module = 0U;
    ucn_i_callback_claim_t claim;
    uint8_t opcode_domain;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || request == NULL ||
        workspace == NULL || packet_output == NULL || packet_bytes == NULL ||
        (plaintext == NULL && plaintext_bytes != 0U) ||
        packet_capacity == 0U ||
        !composition_external_range(runtime, request, sizeof(*request)) ||
        (plaintext_bytes != 0U &&
         !composition_external_range(runtime, plaintext, plaintext_bytes)) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        !composition_external_range(runtime, packet_output,
                                    packet_capacity) ||
        !composition_external_range(runtime, packet_bytes,
                                    sizeof(*packet_bytes)) ||
        ucn_i_ranges_overlap(request, sizeof(*request), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(request, sizeof(*request), packet_output,
                             packet_capacity) ||
        ucn_i_ranges_overlap(request, sizeof(*request), packet_bytes,
                             sizeof(*packet_bytes)) ||
        (plaintext_bytes != 0U &&
         (ucn_i_ranges_overlap(plaintext, plaintext_bytes, workspace,
                               sizeof(*workspace)) ||
          ucn_i_ranges_overlap(plaintext, plaintext_bytes, packet_output,
                               packet_capacity) ||
          ucn_i_ranges_overlap(plaintext, plaintext_bytes, packet_bytes,
                               sizeof(*packet_bytes)))) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), packet_output,
                             packet_capacity) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), packet_bytes,
                             sizeof(*packet_bytes)) ||
        ucn_i_ranges_overlap(packet_output, packet_capacity, packet_bytes,
                             sizeof(*packet_bytes))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_control_tx_body_validate(
        request, plaintext, plaintext_bytes);
    if (result == UCN_OK) {
        result = control_target_module(request->protocol_opcode,
                                       &target_module, &opcode_domain);
    }
    if (result != UCN_OK) {
        return result;
    }
    result = composition_control_enter(runtime, target_module,
                                       UCN_I_COMPOSITION_OP_CONTROL_PREPARE,
                                       &claim);
    if (result != UCN_OK) {
        return result;
    }
    security_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    if (security_owner == NULL) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    result = ucn_i_security_c0_protect(
        security_owner, request, plaintext, plaintext_bytes, workspace,
        packet_output, packet_capacity, packet_bytes);
    (void)opcode_domain;
    return composition_leave(runtime, &claim, result);
}

static ucn_result_t composition_control_tx_submit_impl(
    ucn_i_composition_runtime_t *runtime,
    uint16_t link_slot,
    const ucn_i_security_c0_protect_request_t *request,
    bool use_path,
    ucn_i_security_handle_t hop_session,
    const ucn_i_security_current_facts_t *hop_facts,
    const uint8_t *body,
    size_t body_bytes,
    ucn_i_composition_control_tx_workspace_t *workspace,
    ucn_i_composition_control_tx_plan_t *plan_out)
{
    ucn_i_security_owner_t *security_owner;
    ucn_i_composition_module_id_t target_module = 0U;
    ucn_i_callback_claim_t claim;
    uint32_t link_instance_generation = 0U;
    uint16_t link_mtu = 0U;
    bool link_ready = false;
    bool token_reserved = false;
    uint8_t opcode_domain;
    ucn_result_t result;
    ucn_result_t cleanup_result;

    if (!composition_runtime_header_valid(runtime) || request == NULL ||
        workspace == NULL || plan_out == NULL ||
        (use_path && hop_facts == NULL) ||
        (body == NULL && body_bytes != 0U) ||
        !composition_external_range(runtime, request, sizeof(*request)) ||
        (use_path &&
         !composition_external_range(runtime, hop_facts,
                                     sizeof(*hop_facts))) ||
        (body_bytes != 0U &&
         !composition_external_range(runtime, body, body_bytes)) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        !composition_external_range(runtime, plan_out, sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(request, sizeof(*request), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(request, sizeof(*request), plan_out,
                             sizeof(*plan_out)) ||
        (use_path &&
         (ucn_i_ranges_overlap(request, sizeof(*request), hop_facts,
                               sizeof(*hop_facts)) ||
          ucn_i_ranges_overlap(hop_facts, sizeof(*hop_facts), workspace,
                               sizeof(*workspace)) ||
          ucn_i_ranges_overlap(hop_facts, sizeof(*hop_facts), plan_out,
                               sizeof(*plan_out)))) ||
        (body_bytes != 0U &&
         (ucn_i_ranges_overlap(body, body_bytes, workspace,
                               sizeof(*workspace)) ||
          ucn_i_ranges_overlap(body, body_bytes, plan_out,
                               sizeof(*plan_out)) ||
          (use_path &&
           ucn_i_ranges_overlap(body, body_bytes, hop_facts,
                                sizeof(*hop_facts))))) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), plan_out,
                             sizeof(*plan_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_control_tx_body_validate(
        request, body, body_bytes);
    if (result == UCN_OK) {
        result = control_target_module(request->protocol_opcode,
                                       &target_module, &opcode_domain);
    }
    if (result != UCN_OK) {
        return result;
    }
    result = composition_control_enter(
        runtime, target_module, UCN_I_COMPOSITION_OP_CONTROL_PREPARE, &claim);
    if (result != UCN_OK) {
        return result;
    }
    security_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    if (security_owner == NULL || runtime->foundation_node == NULL) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_adapter_link_snapshot(
        &runtime->foundation_node->adapter, link_slot,
        &link_instance_generation, &link_mtu, &link_ready);
    if (result == UCN_OK &&
        (!link_ready ||
         (use_path ? hop_facts->link_generation :
                     request->facts.link_generation) !=
             link_instance_generation)) {
        result = UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        result = use_path ?
            ucn_i_security_c0_path_protect(
                security_owner, request, hop_session, hop_facts,
                body, body_bytes, &workspace->security,
                workspace->frame, sizeof(workspace->frame),
                &workspace->candidate.frame_bytes) :
            ucn_i_security_c0_protect(
                security_owner, request, body, body_bytes,
                &workspace->security.packet, workspace->frame,
                sizeof(workspace->frame), &workspace->candidate.frame_bytes);
    }
    if (result == UCN_OK && workspace->candidate.frame_bytes > link_mtu) {
        result = UCN_ERR_NO_SPACE;
    }
    if (result == UCN_OK) {
        result = ucn_i_sha256_128(
            workspace->frame, workspace->candidate.frame_bytes,
            workspace->candidate.frame_digest,
            &workspace->digest_workspace);
    }
    if (result == UCN_OK) {
        result = ucn_i_adapter_tx_reserve(
            &runtime->foundation_node->adapter, link_slot,
            link_instance_generation, UINT16_MAX,
            &workspace->candidate.tx_token);
        token_reserved = result == UCN_OK;
    }
    if (result == UCN_OK) {
        result = ucn_i_adapter_tx_submit_ref(
            &runtime->foundation_node->adapter,
            &workspace->candidate.tx_token, workspace->frame, workspace->candidate.frame_bytes,
            &workspace->adapter);
    }
    if (result != UCN_OK) {
        if (token_reserved) {
            cleanup_result = ucn_i_adapter_tx_cancel(
                &runtime->foundation_node->adapter,
                &(workspace->candidate.tx_token));
            if (cleanup_result == UCN_OK) {
                cleanup_result = ucn_i_adapter_tx_retire(
                    &runtime->foundation_node->adapter,
                    &(workspace->candidate.tx_token));
            }
            if (cleanup_result != UCN_OK) {
                composition_mark_fault(runtime);
                result = UCN_ERR_IN_DOUBT;
            }
        }
        return composition_leave(runtime, &claim, result);
    }
    workspace->candidate.struct_size =
        (uint16_t)sizeof(workspace->candidate);
    workspace->candidate.schema = UCN_I_COMPOSITION_CONTROL_SCHEMA;
    workspace->candidate.runtime_instance = runtime->runtime_instance;
    workspace->candidate.frame = workspace->frame;
    workspace->candidate.link_instance_generation =
        link_instance_generation;
    workspace->candidate.link_slot = link_slot;
    result = composition_leave(runtime, &claim, UCN_OK);
    if (result == UCN_OK) {
        *plan_out = workspace->candidate;
    } else {
        cleanup_result = ucn_i_adapter_tx_cancel(
            &runtime->foundation_node->adapter,
            &(workspace->candidate.tx_token));
        if (cleanup_result == UCN_OK) {
            cleanup_result = ucn_i_adapter_tx_retire(
                &runtime->foundation_node->adapter,
                &(workspace->candidate.tx_token));
        }
        if (cleanup_result != UCN_OK) {
            composition_mark_fault(runtime);
            result = UCN_ERR_IN_DOUBT;
        }
    }
    (void)opcode_domain;
    return result;
}

ucn_result_t ucn_i_composition_control_tx_submit(
    ucn_i_composition_runtime_t *runtime,
    uint16_t link_slot,
    const ucn_i_security_c0_protect_request_t *request,
    const uint8_t *body,
    size_t body_bytes,
    ucn_i_composition_control_tx_workspace_t *workspace,
    ucn_i_composition_control_tx_plan_t *plan_out)
{
    static const ucn_i_security_handle_t no_hop_session = {0};

    return composition_control_tx_submit_impl(
        runtime, link_slot, request, false, no_hop_session, NULL,
        body, body_bytes, workspace, plan_out);
}

ucn_result_t ucn_i_composition_control_path_tx_submit(
    ucn_i_composition_runtime_t *runtime,
    uint16_t link_slot,
    const ucn_i_security_c0_protect_request_t *origin_request,
    ucn_i_security_handle_t hop_session,
    const ucn_i_security_current_facts_t *hop_facts,
    const uint8_t *body,
    size_t body_bytes,
    ucn_i_composition_control_tx_workspace_t *workspace,
    ucn_i_composition_control_tx_plan_t *plan_out)
{
    return composition_control_tx_submit_impl(
        runtime, link_slot, origin_request, true, hop_session, hop_facts,
        body, body_bytes, workspace, plan_out);
}

ucn_result_t ucn_i_composition_control_tx_retry(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_tx_plan_t *prior,
    ucn_i_composition_control_tx_workspace_t *workspace,
    ucn_i_composition_control_tx_plan_t *plan_out)
{
    ucn_i_adapter_tx_view_t prior_view;
    ucn_i_callback_claim_t claim;
    ucn_driver_token_t token;
    uint32_t link_instance_generation = 0U;
    uint16_t link_mtu = 0U;
    bool link_ready = false;
    bool token_reserved = false;
    uint8_t digest[UCN_I_DIGEST128_BYTES];
    ucn_result_t result;
    ucn_result_t cleanup_result;

    if (!composition_runtime_header_valid(runtime) || prior == NULL ||
        workspace == NULL || plan_out == NULL ||
        prior->struct_size != sizeof(*prior) ||
        prior->schema != UCN_I_COMPOSITION_CONTROL_SCHEMA ||
        prior->runtime_instance != runtime->runtime_instance ||
        prior->frame == NULL || prior->frame_bytes == 0U ||
        prior->frame_bytes > UCN_ADAPTER_FRAME_BYTES ||
        prior->link_instance_generation == 0U ||
        !bytes_are_zero(prior->reserved_zero,
                        sizeof(prior->reserved_zero)) ||
        !composition_external_range(runtime, prior, sizeof(*prior)) ||
        !composition_external_range(runtime, prior->frame,
                                    prior->frame_bytes) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        !composition_external_range(runtime, plan_out, sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(prior, sizeof(*prior), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(prior, sizeof(*prior), plan_out,
                             sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(prior->frame, prior->frame_bytes, workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(prior->frame, prior->frame_bytes, plan_out,
                             sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), plan_out,
                             sizeof(*plan_out)) ||
        runtime->foundation_node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_control_enter(
        runtime, UCN_I_COMPOSITION_MODULE_COORDINATOR,
        UCN_I_COMPOSITION_OP_CONTROL_PREPARE, &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_sha256_128(prior->frame, prior->frame_bytes, digest,
                              &workspace->digest_workspace);
    if (result != UCN_OK ||
        memcmp(digest, prior->frame_digest, sizeof(digest)) != 0) {
        composition_mark_fault(runtime);
        return composition_leave(runtime, &claim, UCN_ERR_SECURITY);
    }
    result = ucn_i_adapter_tx_view(&runtime->foundation_node->adapter,
                                   &(prior->tx_token), &prior_view);
    if (result == UCN_OK &&
        (prior_view.state != UCN_I_ADAPTER_TX_NOT_SUBMITTED ||
         prior_view.terminal_latched != 0U ||
         prior_view.core_tx_slot != UINT16_MAX)) {
        result = UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        result = ucn_i_adapter_link_snapshot(
            &runtime->foundation_node->adapter, prior->link_slot,
            &link_instance_generation, &link_mtu, &link_ready);
    }
    if (result == UCN_OK &&
        (!link_ready ||
         link_instance_generation != prior->link_instance_generation ||
         prior->frame_bytes > link_mtu)) {
        result = UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        result = ucn_i_adapter_tx_retire(
            &runtime->foundation_node->adapter, &(prior->tx_token));
    }
    memset(&token, 0, sizeof(token));
    if (result == UCN_OK) {
        result = ucn_i_adapter_tx_reserve(
            &runtime->foundation_node->adapter, prior->link_slot,
            link_instance_generation, UINT16_MAX, &token);
        token_reserved = result == UCN_OK;
    }
    if (result == UCN_OK) {
        result = ucn_i_adapter_tx_submit_ref(
            &runtime->foundation_node->adapter, &token, prior->frame,
            prior->frame_bytes, &workspace->adapter);
    }
    if (result != UCN_OK) {
        if (token_reserved) {
            cleanup_result = ucn_i_adapter_tx_cancel(
                &runtime->foundation_node->adapter, &(token));
            if (cleanup_result == UCN_OK) {
                cleanup_result = ucn_i_adapter_tx_retire(
                    &runtime->foundation_node->adapter, &(token));
            }
            if (cleanup_result != UCN_OK) {
                composition_mark_fault(runtime);
                result = UCN_ERR_IN_DOUBT;
            }
        }
        return composition_leave(runtime, &claim, result);
    }
    workspace->candidate = *prior;
    workspace->candidate.tx_token = token;
    result = composition_leave(runtime, &claim, UCN_OK);
    if (result == UCN_OK) {
        *plan_out = workspace->candidate;
    } else {
        cleanup_result = ucn_i_adapter_tx_cancel(
            &runtime->foundation_node->adapter, &(token));
        if (cleanup_result == UCN_OK) {
            cleanup_result = ucn_i_adapter_tx_retire(
                &runtime->foundation_node->adapter, &(token));
        }
        if (cleanup_result != UCN_OK) {
            composition_mark_fault(runtime);
            result = UCN_ERR_IN_DOUBT;
        }
    }
    return result;
}

ucn_result_t ucn_i_composition_control_tx_finish(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_tx_plan_t *plan,
    ucn_i_sha256_workspace_t *workspace)
{
    ucn_i_adapter_tx_view_t view;
    ucn_i_callback_claim_t claim;
    uint8_t digest[UCN_I_DIGEST128_BYTES];
    ucn_result_t result;
    ucn_result_t retire_result;

    if (!composition_runtime_header_valid(runtime) || plan == NULL ||
        workspace == NULL || plan->struct_size != sizeof(*plan) ||
        plan->schema != UCN_I_COMPOSITION_CONTROL_SCHEMA ||
        plan->runtime_instance != runtime->runtime_instance ||
        plan->frame == NULL || plan->frame_bytes == 0U ||
        plan->frame_bytes > UCN_ADAPTER_FRAME_BYTES ||
        plan->link_instance_generation == 0U ||
        !bytes_are_zero(plan->reserved_zero, sizeof(plan->reserved_zero)) ||
        !composition_external_range(runtime, plan, sizeof(*plan)) ||
        !composition_external_range(runtime, plan->frame,
                                    plan->frame_bytes) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(plan->frame, plan->frame_bytes, workspace,
                             sizeof(*workspace)) ||
        runtime->foundation_node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_control_enter(
        runtime, UCN_I_COMPOSITION_MODULE_COORDINATOR,
        UCN_I_COMPOSITION_OP_CONTROL_FINISH, &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_sha256_128(plan->frame, plan->frame_bytes,
                              digest, workspace);
    if (result != UCN_OK ||
        memcmp(digest, plan->frame_digest, sizeof(digest)) != 0) {
        retire_result = ucn_i_adapter_tx_cancel(
            &runtime->foundation_node->adapter, &(plan->tx_token));
        if (retire_result == UCN_OK) {
            retire_result = ucn_i_adapter_tx_retire(
                &runtime->foundation_node->adapter, &(plan->tx_token));
        }
        composition_mark_fault(runtime);
        return composition_leave(
            runtime, &claim,
            retire_result == UCN_OK ? UCN_ERR_SECURITY : UCN_ERR_IN_DOUBT);
    }
    result = ucn_i_adapter_tx_view(
        &runtime->foundation_node->adapter, &(plan->tx_token), &view);
    if (result != UCN_OK) {
        return composition_leave(runtime, &claim, result);
    }
    if (view.core_tx_slot != UINT16_MAX) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    if (view.state == UCN_I_ADAPTER_TX_IN_DOUBT) {
        return composition_leave(runtime, &claim, UCN_ERR_IN_DOUBT);
    }
    if (view.state != UCN_I_ADAPTER_TX_COMPLETED &&
        view.state != UCN_I_ADAPTER_TX_CANCELLED &&
        view.state != UCN_I_ADAPTER_TX_NOT_SUBMITTED) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    result = view.state == UCN_I_ADAPTER_TX_COMPLETED ?
                 view.terminal_result :
             view.state == UCN_I_ADAPTER_TX_CANCELLED ?
                 UCN_ERR_CANCELLED : UCN_ERR_NO_SPACE;
    retire_result = ucn_i_adapter_tx_retire(
        &runtime->foundation_node->adapter, &(plan->tx_token));
    if (retire_result != UCN_OK) {
        composition_mark_fault(runtime);
        result = UCN_ERR_STATE;
    }
    return composition_leave(runtime, &claim, result);
}

static ucn_result_t bootstrap_body_decode(
    const ucn_i_c0_frame_t *frame,
    ucn_i_admission_link_t ingress_link,
    ucn_i_composition_bootstrap_message_t *message_out)
{
    ucn_i_composition_bootstrap_message_t decoded;
    ucn_result_t result;

    if (frame == NULL || message_out == NULL || ingress_link.id == 0U ||
        ingress_link.generation == 0U || ingress_link.reserved_zero != 0U) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&decoded, 0, sizeof(decoded));
    switch (frame->opcode) {
        case UCN_I_OPCODE_BOOTSTRAP_HELLO:
            if (frame->interaction_role != UCN_INTERACTION_REQUEST ||
                frame->payload_bytes != UCN_I_ADMISSION_HELLO_BYTES) {
                return UCN_ERR_MALFORMED;
            }
            result = ucn_i_admission_hello_decode(
                frame->payload, &decoded.hello);
            if (result == UCN_OK && decoded.hello.transaction_id !=
                                      frame->transaction_id) {
                result = UCN_ERR_MALFORMED;
            }
            break;
        case UCN_I_OPCODE_BOOTSTRAP_COOKIE_CHALLENGE:
            if (frame->interaction_role != UCN_INTERACTION_RESULT ||
                frame->payload_bytes !=
                    UCN_I_ADMISSION_COOKIE_CHALLENGE_BYTES) {
                return UCN_ERR_MALFORMED;
            }
            result = ucn_i_admission_cookie_challenge_decode(
                frame->payload, &decoded.cookie_challenge);
            if (result == UCN_OK &&
                decoded.cookie_challenge.transaction_id !=
                    frame->transaction_id) {
                result = UCN_ERR_MALFORMED;
            }
            break;
        case UCN_I_OPCODE_BOOTSTRAP_HELLO_COOKIE:
            if (frame->interaction_role != UCN_INTERACTION_REQUEST) {
                return UCN_ERR_MALFORMED;
            }
            result = ucn_i_admission_hello_cookie_decode(
                frame->payload, frame->payload_bytes,
                ingress_link.id, &decoded.hello_cookie);
            if (result == UCN_OK &&
                (decoded.hello_cookie.key.transaction_id !=
                     frame->transaction_id ||
                 decoded.hello_cookie.key.link.id != ingress_link.id ||
                 decoded.hello_cookie.key.link.generation !=
                     ingress_link.generation)) {
                result = UCN_ERR_MALFORMED;
            }
            break;
        default:
            return UCN_ERR_UNSUPPORTED;
    }
    if (result == UCN_OK) {
        *message_out = decoded;
    }
    return result;
}

static bool bootstrap_plan_message_matches(
    const ucn_i_composition_bootstrap_rx_plan_t *plan)
{
    switch (plan->opcode) {
        case UCN_I_OPCODE_BOOTSTRAP_HELLO:
            return plan->interaction_role == UCN_INTERACTION_REQUEST &&
                   plan->message.hello.transaction_id ==
                       plan->transaction_id;
        case UCN_I_OPCODE_BOOTSTRAP_COOKIE_CHALLENGE:
            return plan->interaction_role == UCN_INTERACTION_RESULT &&
                   plan->message.cookie_challenge.transaction_id ==
                       plan->transaction_id;
        case UCN_I_OPCODE_BOOTSTRAP_HELLO_COOKIE:
            return plan->interaction_role == UCN_INTERACTION_REQUEST &&
                   plan->message.hello_cookie.key.transaction_id ==
                       plan->transaction_id &&
                   plan->message.hello_cookie.key.link.id ==
                       plan->ingress_link.id &&
                   plan->message.hello_cookie.key.link.generation ==
                       plan->ingress_link.generation;
        default:
            return false;
    }
}

static ucn_result_t bootstrap_body_encode(
    uint16_t opcode,
    const ucn_i_composition_bootstrap_message_t *message,
    ucn_i_admission_link_t current_link,
    uint8_t *body,
    size_t body_capacity,
    size_t *body_bytes,
    uint64_t *transaction_id,
    uint8_t *interaction_role)
{
    size_t encoded_bytes = 0U;
    ucn_result_t result;

    if (message == NULL || body == NULL || body_bytes == NULL ||
        transaction_id == NULL || interaction_role == NULL ||
        current_link.id == 0U || current_link.generation == 0U ||
        current_link.reserved_zero != 0U) {
        return UCN_ERR_ARGUMENT;
    }
    switch (opcode) {
        case UCN_I_OPCODE_BOOTSTRAP_HELLO:
            if (body_capacity < UCN_I_ADMISSION_HELLO_BYTES) {
                return UCN_ERR_NO_SPACE;
            }
            result = ucn_i_admission_hello_encode(
                &message->hello, body);
            encoded_bytes = UCN_I_ADMISSION_HELLO_BYTES;
            *transaction_id = message->hello.transaction_id;
            *interaction_role = UCN_INTERACTION_REQUEST;
            break;
        case UCN_I_OPCODE_BOOTSTRAP_COOKIE_CHALLENGE:
            if (body_capacity < UCN_I_ADMISSION_COOKIE_CHALLENGE_BYTES) {
                return UCN_ERR_NO_SPACE;
            }
            if (message->cookie_challenge.issuer_link.id != current_link.id ||
                message->cookie_challenge.issuer_link.generation != current_link.generation) {
                return UCN_ERR_ARGUMENT;
            }
            result = ucn_i_admission_cookie_challenge_encode(
                &message->cookie_challenge, body);
            encoded_bytes = UCN_I_ADMISSION_COOKIE_CHALLENGE_BYTES;
            *transaction_id =
                message->cookie_challenge.transaction_id;
            *interaction_role = UCN_INTERACTION_RESULT;
            break;
        case UCN_I_OPCODE_BOOTSTRAP_HELLO_COOKIE:
            /* Issuer-local scope was learned in Challenge, not the client's TX Link.
             * 签发方域来自Challenge，不得要求等于客户端自己的TX Link。 */
            result = ucn_i_admission_hello_cookie_encode(
                &message->hello_cookie, body, body_capacity,
                &encoded_bytes);
            *transaction_id =
                message->hello_cookie.key.transaction_id;
            *interaction_role = UCN_INTERACTION_REQUEST;
            break;
        default:
            return UCN_ERR_UNSUPPORTED;
    }
    if (result != UCN_OK) {
        return result;
    }
    *body_bytes = encoded_bytes;
    return UCN_OK;
}

static uint32_t bootstrap_reserved_address(uint8_t address_width)
{
    return address_width == 4U ? UINT32_MAX :
        (UINT32_C(1) << (address_width * 8U)) - UINT32_C(1);
}

ucn_result_t ucn_i_composition_bootstrap_tx_submit(
    ucn_i_composition_runtime_t *runtime,
    uint16_t link_slot,
    uint16_t opcode,
    const ucn_i_composition_bootstrap_message_t *message,
    ucn_i_composition_bootstrap_tx_workspace_t *workspace,
    ucn_i_composition_control_tx_plan_t *plan_out)
{
    ucn_i_callback_claim_t claim;
    uint64_t transaction_id = 0U;
    uint32_t link_instance_generation = 0U;
    uint16_t link_mtu = 0U;
    size_t body_bytes = 0U;
    size_t frame_bytes = 0U;
    bool link_ready = false;
    bool token_reserved = false;
    uint8_t interaction_role = 0U;
    ucn_result_t result;
    ucn_result_t cleanup_result;

    if (!composition_runtime_header_valid(runtime) || message == NULL ||
        workspace == NULL || plan_out == NULL ||
        !composition_external_range(runtime, message, sizeof(*message)) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        !composition_external_range(runtime, plan_out, sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(message, sizeof(*message), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(message, sizeof(*message), plan_out,
                             sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), plan_out,
                             sizeof(*plan_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_bootstrap_enter(
        runtime, UCN_I_COMPOSITION_OP_BOOTSTRAP_TX, &claim);
    if (result != UCN_OK) {
        return result;
    }
    if (runtime->foundation_node == NULL) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    if ((runtime->plan.capability_mask & UCN_I_CAP_DYNAMIC_ADMISSION) == 0U ||
        (runtime->plan.selected_module_mask & UCN_I_COMPOSE_ADMISSION) == 0U) {
        return composition_leave(runtime, &claim, UCN_ERR_UNSUPPORTED);
    }
    if (!composition_owner_is_active(
            runtime, UCN_I_COMPOSITION_MODULE_ADMISSION)) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_adapter_link_snapshot(
        &runtime->foundation_node->adapter, link_slot,
        &link_instance_generation, &link_mtu, &link_ready);
    if (result == UCN_OK && !link_ready) {
        result = UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        workspace->current_link.id = (uint16_t)(link_slot + 1U);
        workspace->current_link.generation = link_instance_generation;
        result = bootstrap_body_encode(
            opcode, message, workspace->current_link, workspace->body,
            sizeof(workspace->body), &body_bytes, &transaction_id,
            &interaction_role);
    }
    if (result == UCN_OK) {
        workspace->c0_frame.payload = workspace->body;
        workspace->c0_frame.payload_bytes = body_bytes;
        workspace->c0_frame.transaction_id = transaction_id;
        workspace->c0_frame.realm_id = runtime->foundation_node->realm_id;
        workspace->c0_frame.source_address = 0U;
        workspace->c0_frame.destination_address = bootstrap_reserved_address(
            runtime->foundation_node->address_width);
        workspace->c0_frame.opcode = opcode;
        workspace->c0_frame.traffic_class = UCN_TRAFFIC_Q0;
        workspace->c0_frame.delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
        workspace->c0_frame.interaction_role = interaction_role;
        workspace->c0_frame.payload_kind = UCN_I_PAYLOAD_CONTROL;
        workspace->c0_frame.origin_security = UCN_I_ORIGIN_O0;
        workspace->c0_frame.hop_limit = 1U;
        result = ucn_i_c0_encode(
            &workspace->c0_frame, runtime->foundation_node->address_width,
            UCN_I_CONTROL_H0, workspace->frame,
            sizeof(workspace->frame), &frame_bytes);
    }
    if (result == UCN_OK && frame_bytes > link_mtu) {
        result = UCN_ERR_NO_SPACE;
    }
    if (result == UCN_OK) {
        result = ucn_i_sha256_128(
            workspace->frame, frame_bytes,
            workspace->candidate.frame_digest,
            &workspace->digest_workspace);
    }
    if (result == UCN_OK) {
        result = ucn_i_adapter_tx_reserve(
            &runtime->foundation_node->adapter, link_slot,
            link_instance_generation, UINT16_MAX, &workspace->token);
        token_reserved = result == UCN_OK;
    }
    if (result == UCN_OK) {
        result = ucn_i_adapter_tx_submit_ref(
            &runtime->foundation_node->adapter, &workspace->token,
            workspace->frame, frame_bytes, &workspace->adapter);
    }
    if (result != UCN_OK) {
        if (token_reserved) {
            cleanup_result = ucn_i_adapter_tx_cancel(
                &runtime->foundation_node->adapter, &(workspace->token));
            if (cleanup_result == UCN_OK) {
                cleanup_result = ucn_i_adapter_tx_retire(
                    &runtime->foundation_node->adapter, &(workspace->token));
            }
            if (cleanup_result != UCN_OK) {
                composition_mark_fault(runtime);
                result = UCN_ERR_IN_DOUBT;
            }
        }
        return composition_leave(runtime, &claim, result);
    }
    workspace->candidate.struct_size =
        (uint16_t)sizeof(workspace->candidate);
    workspace->candidate.schema = UCN_I_COMPOSITION_CONTROL_SCHEMA;
    workspace->candidate.runtime_instance = runtime->runtime_instance;
    workspace->candidate.frame = workspace->frame;
    workspace->candidate.frame_bytes = frame_bytes;
    workspace->candidate.tx_token = workspace->token;
    workspace->candidate.link_instance_generation =
        link_instance_generation;
    workspace->candidate.link_slot = link_slot;
    result = composition_leave(runtime, &claim, UCN_OK);
    if (result == UCN_OK) {
        *plan_out = workspace->candidate;
    } else {
        cleanup_result = ucn_i_adapter_tx_cancel(
            &runtime->foundation_node->adapter, &(workspace->token));
        if (cleanup_result == UCN_OK) {
            cleanup_result = ucn_i_adapter_tx_retire(
                &runtime->foundation_node->adapter, &(workspace->token));
        }
        if (cleanup_result != UCN_OK) {
            composition_mark_fault(runtime);
            result = UCN_ERR_IN_DOUBT;
        }
    }
    return result;
}

static ucn_result_t composition_bootstrap_rx_claim_prepare_locked(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_bootstrap_rx_workspace_t *workspace)
{
    bool admission_present = false;
    ucn_result_t result;
    ucn_result_t retire_result;

    if (runtime->foundation_node == NULL) {
        return UCN_ERR_STATE;
    }
    memset(workspace, 0, sizeof(*workspace));
    {
        const ucn_i_composition_owner_record_t *record = owner_record_find(
            runtime, UCN_I_COMPOSITION_MODULE_ADMISSION);
        if(record != NULL) {
            workspace->candidate.target = record->ref;
            admission_present = true;
        }
    }
    result = ucn_i_adapter_rx_claim_contract_origin_range(
        &runtime->foundation_node->adapter, UCN_I_WIRE_PROTOCOL_VERSION,
        UCN_I_WIRE_CONTRACT_C0,
        UCN_I_ORIGIN_O0, UCN_I_ORIGIN_O0, &workspace->rx);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_adapter_link_snapshot(
        &runtime->foundation_node->adapter, workspace->rx.link_slot,
        &workspace->prepare.link_generation, &workspace->prepare.link_mtu, &workspace->prepare.link_ready);
    if (result == UCN_OK &&
        (!workspace->prepare.link_ready || workspace->prepare.link_generation !=
                            workspace->rx.link_instance_generation ||
         workspace->rx.frame_bytes > workspace->prepare.link_mtu)) {
        result = UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        result = ucn_i_c0_decode(
            workspace->rx.frame, workspace->rx.frame_bytes,
            runtime->foundation_node->address_width,
            UCN_I_CONTROL_H0, &workspace->frame);
    }
    if (result == UCN_OK &&
        (workspace->frame.origin_security != UCN_I_ORIGIN_O0 ||
         workspace->frame.realm_id !=
             runtime->foundation_node->realm_id ||
         workspace->frame.traffic_class != UCN_TRAFFIC_Q0 ||
         workspace->frame.delivery_guarantee !=
             UCN_DELIVERY_BEST_EFFORT ||
         workspace->frame.payload_kind != UCN_I_PAYLOAD_CONTROL ||
         workspace->frame.hop_limit != 1U)) {
        result = UCN_ERR_MALFORMED;
    }
    if (result == UCN_OK &&
        (((runtime->plan.capability_mask &
           UCN_I_CAP_DYNAMIC_ADMISSION) == 0U) ||
         ((runtime->plan.selected_module_mask &
           UCN_I_COMPOSE_ADMISSION) == 0U))) {
        result = UCN_ERR_UNSUPPORTED;
    }
    if (result == UCN_OK &&
        (!admission_present ||
         owner_storage(runtime, UCN_I_COMPOSITION_MODULE_ADMISSION) == NULL ||
         !composition_owner_is_active(
             runtime, UCN_I_COMPOSITION_MODULE_ADMISSION))) {
        result = UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        workspace->candidate.ingress_link.id =
            (uint16_t)(workspace->rx.link_slot + 1U);
        workspace->candidate.ingress_link.generation =
            workspace->rx.link_instance_generation;
        result = bootstrap_body_decode(
            &workspace->frame, workspace->candidate.ingress_link,
            &workspace->candidate.message);
    }
    if (result == UCN_OK) {
        workspace->candidate.struct_size =
            (uint16_t)sizeof(workspace->candidate);
        workspace->candidate.schema = UCN_I_COMPOSITION_BOOTSTRAP_SCHEMA;
        workspace->candidate.runtime_instance = runtime->runtime_instance;
        workspace->candidate.rx_token = workspace->rx.token;
        workspace->candidate.transaction_id =
            workspace->frame.transaction_id;
        workspace->candidate.realm_id = workspace->frame.realm_id;
        workspace->candidate.link_instance_generation =
            workspace->rx.link_instance_generation;
        workspace->candidate.opcode = workspace->frame.opcode;
        workspace->candidate.link_slot = workspace->rx.link_slot;
        workspace->candidate.interaction_role =
            workspace->frame.interaction_role;
        result = ucn_i_sha256_128(
            (const uint8_t *)&workspace->candidate.message,
            sizeof(workspace->candidate.message),
            workspace->candidate.message_digest,
            &workspace->digest_workspace);
    }
    if (result != UCN_OK) {
        retire_result = ucn_i_adapter_rx_retire(
            &runtime->foundation_node->adapter, &(workspace->rx.token));
        if (retire_result != UCN_OK) {
            composition_mark_fault(runtime);
            result = UCN_ERR_STATE;
        }
        return result;
    }
    return UCN_OK;
}

ucn_result_t ucn_i_composition_bootstrap_rx_claim_prepare(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_bootstrap_rx_workspace_t *workspace,
    ucn_i_composition_bootstrap_rx_plan_t *plan_out)
{
    ucn_i_callback_claim_t claim;
    ucn_result_t locked_result;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || workspace == NULL ||
        plan_out == NULL ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        !composition_external_range(runtime, plan_out, sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), plan_out,
                             sizeof(*plan_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_bootstrap_enter(
        runtime, UCN_I_COMPOSITION_OP_BOOTSTRAP_PREPARE, &claim);
    if (result != UCN_OK) {
        return result;
    }
    locked_result =
        composition_bootstrap_rx_claim_prepare_locked(runtime, workspace);
    result = composition_leave(runtime, &claim, locked_result);
    if (result == UCN_OK) {
        *plan_out = workspace->candidate;
    } else if (locked_result == UCN_OK &&
               !composition_token_is_zero(workspace->rx.token)) {
        (void)ucn_i_adapter_rx_retire(
            &runtime->foundation_node->adapter, &(workspace->rx.token));
    }
    return result;
}

static ucn_result_t composition_bootstrap_rx_finish_locked(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_bootstrap_rx_plan_t *plan,
    ucn_i_sha256_workspace_t *workspace,
    bool consumed)
{
    const ucn_i_composition_owner_record_t *admission_record;
    uint8_t digest[UCN_I_DIGEST128_BYTES];
    uint32_t current_link_instance = 0U;
    uint16_t current_link_mtu = 0U;
    bool link_ready = false;
    ucn_result_t result;
    ucn_result_t retire_result;

    admission_record = owner_record_find(
        runtime, UCN_I_COMPOSITION_MODULE_ADMISSION);
    if (admission_record == NULL ||
        !owner_ref_equal(&plan->target, &admission_record->ref)) {
        result = UCN_ERR_STATE;
    } else if (!bootstrap_plan_message_matches(plan)) {
        result = UCN_ERR_SECURITY;
        composition_mark_fault(runtime);
    } else {
        result = ucn_i_adapter_link_snapshot(
            &runtime->foundation_node->adapter, plan->link_slot,
            &current_link_instance, &current_link_mtu, &link_ready);
        if (result == UCN_OK &&
            (!link_ready || current_link_instance !=
                                plan->link_instance_generation)) {
            result = UCN_ERR_STATE;
        }
    }
    if (result == UCN_OK) {
        result = ucn_i_sha256_128(
            (const uint8_t *)&plan->message, sizeof(plan->message),
            digest, workspace);
        if (result == UCN_OK &&
            memcmp(digest, plan->message_digest, sizeof(digest)) != 0) {
            result = UCN_ERR_SECURITY;
            composition_mark_fault(runtime);
        }
    }
    retire_result = ucn_i_adapter_rx_retire(
        &runtime->foundation_node->adapter, &(plan->rx_token));
    if (retire_result != UCN_OK) {
        composition_mark_fault(runtime);
        result = UCN_ERR_STATE;
    }
    (void)consumed;
    (void)current_link_mtu;
    return result;
}

ucn_result_t ucn_i_composition_bootstrap_rx_finish(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_bootstrap_rx_plan_t *plan,
    ucn_i_sha256_workspace_t *workspace,
    bool consumed)
{
    ucn_i_callback_claim_t claim;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || plan == NULL ||
        workspace == NULL || plan->struct_size != sizeof(*plan) ||
        plan->schema != UCN_I_COMPOSITION_BOOTSTRAP_SCHEMA ||
        plan->runtime_instance != runtime->runtime_instance ||
        plan->transaction_id == 0U || plan->realm_id == 0U ||
        plan->link_instance_generation == 0U ||
        plan->ingress_link.id == 0U ||
        plan->ingress_link.generation !=
            plan->link_instance_generation ||
        plan->ingress_link.reserved_zero != 0U ||
        !bytes_are_zero(plan->reserved_zero,
                        sizeof(plan->reserved_zero)) ||
        !composition_external_range(runtime, plan, sizeof(*plan)) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), workspace,
                             sizeof(*workspace)) ||
        runtime->foundation_node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_bootstrap_enter(
        runtime, UCN_I_COMPOSITION_OP_BOOTSTRAP_FINISH, &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = composition_bootstrap_rx_finish_locked(
        runtime, plan, workspace, consumed);
    return composition_leave(runtime, &claim, result);
}

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
    ucn_i_composition_control_dispatch_plan_t *plan_out)
{
    ucn_i_security_owner_t *security_owner;
    size_t candidate_bytes = 0U;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || access == NULL ||
        current_facts == NULL || packet == NULL || packet_bytes == 0U ||
        workspace == NULL || body_output == NULL || body_bytes == NULL ||
        plan_out == NULL || body_capacity == 0U ||
        !composition_external_range(runtime, access, sizeof(*access)) ||
        !composition_external_range(runtime, current_facts,
                                    sizeof(*current_facts)) ||
        !composition_external_range(runtime, packet, packet_bytes) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        !composition_external_range(runtime, body_output, body_capacity) ||
        !composition_external_range(runtime, body_bytes,
                                    sizeof(*body_bytes)) ||
        !composition_external_range(runtime, plan_out, sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), packet,
                             packet_bytes) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), body_output,
                             body_capacity) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), body_bytes,
                             sizeof(*body_bytes)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), plan_out,
                             sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(packet, packet_bytes, body_output,
                             body_capacity) ||
        ucn_i_ranges_overlap(packet, packet_bytes, body_bytes,
                             sizeof(*body_bytes)) ||
        ucn_i_ranges_overlap(packet, packet_bytes, plan_out,
                             sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(body_output, body_capacity, body_bytes,
                             sizeof(*body_bytes)) ||
        ucn_i_ranges_overlap(body_output, body_capacity, plan_out,
                             sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(body_bytes, sizeof(*body_bytes), plan_out,
                             sizeof(*plan_out))) {
        return UCN_ERR_ARGUMENT;
    }
    security_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    if (!ucn_i_composition_runtime_valid(runtime) ||
        runtime->phase != UCN_I_COMPOSITION_RUNNING ||
        !composition_owner_is_active(
            runtime, UCN_I_COMPOSITION_MODULE_SECURITY) ||
        security_owner == NULL) {
        return UCN_ERR_STATE;
    }
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_security_c0_open(
        security_owner, access, current_facts, packet, packet_bytes,
        &workspace->security.packet, workspace->plaintext,
        sizeof(workspace->plaintext), &candidate_bytes,
        &workspace->authenticated);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_composition_control_dispatch_prepare(
        runtime, &workspace->authenticated, current_facts,
        &workspace->validate, &workspace->plan);
    if (result == UCN_OK && candidate_bytes > body_capacity) {
        result = UCN_ERR_NO_SPACE;
    }
    if (result != UCN_OK) {
        (void)ucn_i_security_replay_abort(
            security_owner, &(workspace->authenticated.replay));
        return result;
    }
    if (candidate_bytes != 0U) {
        memcpy(body_output, workspace->plaintext, candidate_bytes);
    }
    workspace->plan.authenticated.body = body_output;
    *body_bytes = candidate_bytes;
    *plan_out = workspace->plan;
    return UCN_OK;
}

static ucn_result_t composition_control_rx_open_claimed_locked(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    ucn_i_composition_control_rx_workspace_t *workspace)
{
    ucn_result_t result;
    ucn_result_t retire_result;

    workspace->prepare.security_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    if (workspace->prepare.security_owner == NULL ||
        runtime->foundation_node == NULL) {
        return UCN_ERR_STATE;
    }
    if (workspace->rx.frame == NULL || workspace->rx.frame_bytes == 0U ||
        workspace->rx.link_instance_generation == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    workspace->prepare.candidate_bytes = 0U;
    result = ucn_i_adapter_link_snapshot(
        &runtime->foundation_node->adapter, workspace->rx.link_slot,
        &workspace->prepare.current_link_instance,
        &workspace->prepare.current_link_mtu,
        &workspace->prepare.link_ready);
    if (result == UCN_OK &&
        (workspace->prepare.link_ready == 0U ||
         workspace->prepare.current_link_instance !=
                            workspace->rx.link_instance_generation ||
         workspace->rx.frame_bytes > workspace->prepare.current_link_mtu)) {
        result = UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        result = ucn_i_security_c0_receive_context_resolve(
            workspace->prepare.security_owner, workspace->rx.frame,
            workspace->rx.frame_bytes,
            now_us, workspace->rx.link_instance_generation,
            &workspace->access, &workspace->facts);
        workspace->prepare.direct_context = result == UCN_OK ? 1U : 0U;
    }
    if (result == UCN_ERR_NOT_FOUND) {
        result = ucn_i_security_c0_origin_receive_context_resolve(
            workspace->prepare.security_owner, workspace->rx.frame,
            workspace->rx.frame_bytes, now_us,
            &workspace->origin_resolve, &workspace->access,
            &workspace->facts);
        workspace->prepare.has_hop =
            result == UCN_OK &&
            workspace->origin_resolve.frame.hop_tag != NULL ? 1U : 0U;
    }
    if (result == UCN_OK && workspace->prepare.has_hop != 0U &&
        workspace->prepare.direct_context == 0U) {
        result = ucn_i_security_c0_hop_receive_context_resolve(
            workspace->prepare.security_owner, workspace->rx.frame,
            workspace->rx.frame_bytes,
            now_us, workspace->rx.link_instance_generation,
            &workspace->hop_session, &workspace->hop_facts);
    }
    if (result == UCN_OK && workspace->prepare.has_hop != 0U &&
        workspace->prepare.direct_context == 0U) {
        result = ucn_i_security_c0_path_open(
            workspace->prepare.security_owner, &workspace->access,
            &workspace->facts,
            workspace->hop_session, &workspace->hop_facts,
            workspace->rx.frame, workspace->rx.frame_bytes,
            &workspace->control.security, workspace->control.plaintext,
            sizeof(workspace->control.plaintext),
            &workspace->prepare.candidate_bytes,
            &workspace->control.authenticated, &workspace->hop_replay);
        workspace->prepare.replay_pending = result == UCN_OK ? 1U : 0U;
        workspace->prepare.hop_replay_pending =
            result == UCN_OK ? 1U : 0U;
    } else if (result == UCN_OK) {
        result = ucn_i_security_c0_open(
            workspace->prepare.security_owner, &workspace->access,
            &workspace->facts,
            workspace->rx.frame, workspace->rx.frame_bytes,
            &workspace->control.security.packet,
            workspace->control.plaintext,
            sizeof(workspace->control.plaintext),
            &workspace->prepare.candidate_bytes,
            &workspace->control.authenticated);
        workspace->prepare.replay_pending = result == UCN_OK ? 1U : 0U;
    }
    if (result == UCN_OK) {
        result = composition_control_dispatch_prepare_locked(
            runtime, &workspace->control.authenticated, &workspace->facts,
            &workspace->control.validate, &workspace->control.plan);
    }
    if (result == UCN_OK &&
        workspace->prepare.candidate_bytes >
            workspace->prepare.body_capacity) {
        result = UCN_ERR_NO_SPACE;
    }
    if (result != UCN_OK) {
        if (workspace->prepare.replay_pending != 0U) {
            (void)ucn_i_security_replay_abort(
                workspace->prepare.security_owner,
                &(workspace->control.authenticated.replay));
        }
        if (workspace->prepare.hop_replay_pending != 0U) {
            (void)ucn_i_security_hop_replay_abort(
                workspace->prepare.security_owner, &(workspace->hop_replay));
        }
        retire_result = ucn_i_adapter_rx_retire(
            &runtime->foundation_node->adapter, &(workspace->rx.token));
        if (retire_result != UCN_OK) {
            composition_mark_fault(runtime);
            result = UCN_ERR_STATE;
        }
        return result;
    }
    if (workspace->prepare.candidate_bytes != 0U) {
        memcpy(workspace->prepare.body_output,
               workspace->control.plaintext,
               workspace->prepare.candidate_bytes);
    }
    workspace->control.plan.authenticated.body =
        workspace->prepare.body_output;
    memset(&workspace->candidate, 0, sizeof(workspace->candidate));
    workspace->candidate.struct_size =
        (uint16_t)sizeof(workspace->candidate);
    workspace->candidate.schema = UCN_I_COMPOSITION_CONTROL_SCHEMA;
    workspace->candidate.runtime_instance = runtime->runtime_instance;
    workspace->candidate.dispatch = workspace->control.plan;
    workspace->candidate.facts = workspace->facts;
    workspace->candidate.hop_facts = workspace->hop_facts;
    workspace->candidate.hop_replay = workspace->hop_replay;
    workspace->candidate.rx_token = workspace->rx.token;
    workspace->candidate.link_instance_generation =
        workspace->rx.link_instance_generation;
    workspace->candidate.link_slot = workspace->rx.link_slot;
    workspace->candidate.has_hop_replay = workspace->prepare.has_hop;
    *workspace->prepare.candidate_bytes_out =
        workspace->prepare.candidate_bytes;
    workspace->prepare.body_output = NULL;
    workspace->prepare.candidate_bytes_out = NULL;
    workspace->prepare.security_owner = NULL;
    workspace->prepare.body_capacity = 0U;
    workspace->prepare.claim_rx = false;
    return UCN_OK;
}

static ucn_result_t composition_control_rx_claim_prepare_locked(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    ucn_i_composition_control_rx_workspace_t *workspace)
{
    ucn_result_t result;

    if (!workspace->prepare.claim_rx) {
        return composition_control_rx_open_claimed_locked(
            runtime, now_us, workspace);
    }
    result = ucn_i_adapter_rx_claim_contract_origin_range(
        &runtime->foundation_node->adapter, UCN_I_WIRE_PROTOCOL_VERSION,
        UCN_I_WIRE_CONTRACT_C0, UCN_I_ORIGIN_O1, UCN_I_ORIGIN_O2,
        &workspace->rx);
    if (result != UCN_OK) {
        return result;
    }
    return composition_control_rx_open_claimed_locked(
        runtime, now_us, workspace);
}

ucn_result_t ucn_i_composition_control_rx_claim_prepare(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    ucn_i_composition_control_rx_workspace_t *workspace,
    uint8_t *body_output,
    size_t body_capacity,
    size_t *body_bytes,
    ucn_i_composition_control_rx_plan_t *plan_out)
{
    ucn_i_security_owner_t *security_owner;
    ucn_i_callback_claim_t claim;
    ucn_result_t locked_result;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || now_us == 0U ||
        workspace == NULL || body_output == NULL || body_capacity == 0U ||
        body_bytes == NULL || plan_out == NULL ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        !composition_external_range(runtime, body_output, body_capacity) ||
        !composition_external_range(runtime, body_bytes,
                                    sizeof(*body_bytes)) ||
        !composition_external_range(runtime, plan_out, sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), body_output,
                             body_capacity) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), body_bytes,
                             sizeof(*body_bytes)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), plan_out,
                             sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(body_output, body_capacity, body_bytes,
                             sizeof(*body_bytes)) ||
        ucn_i_ranges_overlap(body_output, body_capacity, plan_out,
                             sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(body_bytes, sizeof(*body_bytes), plan_out,
                             sizeof(*plan_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_control_enter(
        runtime, UCN_I_COMPOSITION_MODULE_COORDINATOR,
        UCN_I_COMPOSITION_OP_CONTROL_PREPARE, &claim);
    if (result != UCN_OK) {
        return result;
    }
    memset(workspace, 0, sizeof(*workspace));
    workspace->prepare.body_output = body_output;
    workspace->prepare.candidate_bytes_out = body_bytes;
    workspace->prepare.body_capacity = body_capacity;
    workspace->prepare.claim_rx = true;
    locked_result = composition_control_rx_claim_prepare_locked(
        runtime, now_us, workspace);
    result = composition_leave(runtime, &claim, locked_result);
    if (result == UCN_OK) {
        *plan_out = workspace->candidate;
    } else if (locked_result == UCN_OK) {
        security_owner = owner_storage(
            runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
        if (security_owner != NULL) {
            (void)ucn_i_security_replay_abort(
                security_owner, &(workspace->control.authenticated.replay));
            if (workspace->candidate.has_hop_replay != 0U) {
                (void)ucn_i_security_hop_replay_abort(
                    security_owner, &(workspace->candidate.hop_replay));
            }
        }
        (void)ucn_i_adapter_rx_retire(
            &runtime->foundation_node->adapter, &(workspace->rx.token));
    }
    return result;
}

static ucn_result_t composition_control_rx_finish_locked(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_rx_plan_t *plan,
    ucn_i_security_control_validate_workspace_t *workspace,
    bool consumed)
{
    ucn_i_security_owner_t *security_owner;
    uint32_t current_link_instance = 0U;
    uint16_t current_link_mtu = 0U;
    bool link_ready = false;
    ucn_result_t result;
    ucn_result_t retire_result;
    ucn_result_t hop_result = UCN_OK;

    security_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    if (security_owner == NULL) {
        return UCN_ERR_STATE;
    }
    result = ucn_i_adapter_link_snapshot(
        &runtime->foundation_node->adapter, plan->link_slot,
        &current_link_instance, &current_link_mtu, &link_ready);
    if (result != UCN_OK || !link_ready ||
        current_link_instance != plan->link_instance_generation ||
        (plan->has_hop_replay != 0U ?
             plan->hop_facts.link_generation :
             plan->facts.link_generation) !=
            plan->link_instance_generation) {
        (void)composition_control_dispatch_finish_locked(
            runtime, &plan->dispatch, &plan->facts, workspace, false);
        if (plan->has_hop_replay != 0U) {
            (void)ucn_i_security_hop_replay_abort(
                security_owner, &(plan->hop_replay));
        }
        result = UCN_ERR_STATE;
    } else {
        result = composition_control_dispatch_finish_locked(
            runtime, &plan->dispatch, &plan->facts, workspace, consumed);
        if (plan->has_hop_replay != 0U) {
            hop_result = result == UCN_OK && consumed ?
                ucn_i_security_hop_replay_commit(
                    security_owner, &(plan->hop_replay), &plan->hop_facts) :
                ucn_i_security_hop_replay_abort(
                    security_owner, &(plan->hop_replay));
            if (hop_result != UCN_OK) {
                composition_mark_fault(runtime);
                result = UCN_ERR_IN_DOUBT;
            }
        }
    }
    retire_result = ucn_i_adapter_rx_retire(
        &runtime->foundation_node->adapter, &(plan->rx_token));
    if (retire_result != UCN_OK) {
        composition_mark_fault(runtime);
        return UCN_ERR_STATE;
    }
    (void)current_link_mtu;
    return result;
}

static bool composition_control_rx_hop_state_valid(
    const ucn_i_composition_control_rx_plan_t *plan)
{
    if (plan->has_hop_replay == 0U) {
        return bytes_are_zero(&plan->hop_facts,
                              sizeof(plan->hop_facts)) &&
               bytes_are_zero(&plan->hop_replay,
                              sizeof(plan->hop_replay));
    }
    return plan->has_hop_replay == 1U &&
           plan->hop_facts.now_us == plan->facts.now_us &&
           plan->hop_facts.link_generation ==
               plan->link_instance_generation &&
           plan->hop_replay.session.runtime_instance ==
               plan->runtime_instance &&
           plan->hop_replay.session.owner_instance != 0U &&
           plan->hop_replay.session.slot_generation != 0U &&
           plan->hop_replay.session.session_generation != 0U &&
           plan->hop_replay.hop_sequence != 0U &&
           plan->hop_replay.reservation_generation != 0U &&
           plan->hop_replay.reserved_zero == 0U;
}

ucn_result_t ucn_i_composition_control_rx_finish(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_control_rx_plan_t *plan,
    ucn_i_security_control_validate_workspace_t *workspace,
    bool consumed)
{
    ucn_i_composition_module_id_t target_module;
    ucn_i_callback_claim_t claim;
    uint8_t opcode_domain;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || plan == NULL ||
        workspace == NULL || plan->struct_size != sizeof(*plan) ||
        plan->schema != UCN_I_COMPOSITION_CONTROL_SCHEMA ||
        plan->runtime_instance != runtime->runtime_instance ||
        plan->link_instance_generation == 0U ||
        plan->has_hop_replay > 1U || plan->reserved_zero != 0U ||
        !composition_control_rx_hop_state_valid(plan) ||
        !composition_external_range(runtime, plan, sizeof(*plan)) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), workspace,
                             sizeof(*workspace)) ||
        runtime->foundation_node == NULL ||
        plan->dispatch.struct_size != sizeof(plan->dispatch) ||
        plan->dispatch.schema != UCN_I_COMPOSITION_CONTROL_SCHEMA ||
        plan->dispatch.runtime_instance != runtime->runtime_instance ||
        !bytes_are_zero(plan->dispatch.reserved_zero,
                        sizeof(plan->dispatch.reserved_zero))) {
        return UCN_ERR_ARGUMENT;
    }
    result = control_target_module(
        plan->dispatch.authenticated.protocol_opcode,
        &target_module, &opcode_domain);
    if (result != UCN_OK ||
        plan->dispatch.target_module != (uint8_t)target_module ||
        plan->dispatch.opcode_domain != opcode_domain) {
        return result == UCN_OK ? UCN_ERR_STATE : result;
    }
    result = composition_control_enter(
        runtime, target_module, UCN_I_COMPOSITION_OP_CONTROL_FINISH, &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = composition_control_rx_finish_locked(
        runtime, plan, workspace, consumed);
    return composition_leave(runtime, &claim, result);
}

static void composition_ingress_result_fill(
    const ucn_i_composition_runtime_t *runtime,
    uint16_t operations,
    ucn_i_composition_ingress_kind_t selected_kind,
    bool more_work,
    ucn_i_composition_ingress_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->struct_size = (uint16_t)sizeof(*result);
    result->schema = UCN_I_COMPOSITION_INGRESS_SCHEMA;
    result->runtime_instance = runtime->runtime_instance;
    result->generation = runtime->ingress_generation;
    result->operations = operations;
    result->selected_kind = selected_kind;
    result->more_work = more_work ? 1U : 0U;
    result->next_cursor = runtime->ingress_cursor;
}

static void composition_ingress_pending_publish(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_ingress_workspace_t *workspace,
    ucn_i_composition_ingress_kind_t kind,
    const ucn_driver_token_t *token)
{
    ++runtime->ingress_generation;
    runtime->ingress_pending_kind = kind;
    runtime->ingress_pending_token = *token;
    workspace->struct_size = (uint16_t)sizeof(*workspace);
    workspace->schema = UCN_I_COMPOSITION_INGRESS_SCHEMA;
    workspace->runtime_instance = runtime->runtime_instance;
    workspace->generation = runtime->ingress_generation;
    workspace->active_kind = kind;
}

static const ucn_i_route_link_ref_t *composition_relay_next_hop(
    const ucn_i_route_resolved_t *route)
{
    if (route->kind == UCN_I_ROUTE_RESOLVED_DYNAMIC) {
        return &route->dynamic.next_hop;
    }
    if (route->kind == UCN_I_ROUTE_RESOLVED_STATIC) {
        return &route->static_route.next_hop;
    }
    return NULL;
}

static ucn_result_t composition_control_relay_prepare_claimed_locked(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    ucn_i_composition_ingress_workspace_t *workspace)
{
    ucn_i_protocol_opcode_support_t opcode_support = 0U;
    const ucn_i_route_link_ref_t *next_hop;
    ucn_i_security_owner_t *security_owner;
    ucn_i_route_owner_t *route_owner;
    uint32_t ingress_link_generation = 0U;
    uint32_t egress_link_generation = 0U;
    uint16_t ingress_mtu = 0U;
    uint16_t egress_mtu = 0U;
    bool ingress_ready = false;
    bool egress_ready = false;
    bool replay_pending = false;
    bool tx_reserved = false;
    ucn_result_t result;
    ucn_result_t cleanup_result;

    security_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    route_owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_ROUTE);
    if (security_owner == NULL || route_owner == NULL ||
        !composition_owner_is_active(
            runtime, UCN_I_COMPOSITION_MODULE_SECURITY) ||
        !composition_owner_is_active(
            runtime, UCN_I_COMPOSITION_MODULE_ROUTE) ||
        (runtime->plan.capability_mask & UCN_I_CAP_AUTO_ROUTE) == 0U ||
        runtime->foundation_node == NULL ||
        workspace->control.rx.frame == NULL ||
        workspace->control.rx.frame_bytes == 0U) {
        return UCN_ERR_STATE;
    }
    result = ucn_i_adapter_link_snapshot(
        &runtime->foundation_node->adapter,
        workspace->control.rx.link_slot, &ingress_link_generation,
        &ingress_mtu, &ingress_ready);
    if (result == UCN_OK &&
        (!ingress_ready || ingress_link_generation !=
             workspace->control.rx.link_instance_generation ||
         workspace->control.rx.frame_bytes > ingress_mtu)) {
        result = UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        result = ucn_i_security_c0_hop_receive_context_resolve(
            security_owner, workspace->control.rx.frame,
            workspace->control.rx.frame_bytes, now_us,
            workspace->control.rx.link_instance_generation,
            &workspace->control.hop_session,
            &workspace->relay_inbound_facts);
    }
    if (result == UCN_OK) {
        result = ucn_i_security_c0_hop_open(
            security_owner, workspace->control.hop_session,
            &workspace->relay_inbound_facts,
            workspace->control.rx.frame,
            workspace->control.rx.frame_bytes,
            &workspace->control.control.security);
        if (result == UCN_OK) {
            result = ucn_i_protocol_opcode_support(
                workspace->control.control.security.relay.protocol_opcode,
                &opcode_support);
        }
        if (result == UCN_OK &&
            (opcode_support & UCN_I_OPCODE_SUPPORT_RELAY) == 0U) {
            result = UCN_ERR_UNSUPPORTED;
        }
        if (result == UCN_OK) {
            workspace->relay_fact =
                workspace->control.control.security.relay;
        }
        replay_pending = result == UCN_OK;
    }
    if (result == UCN_OK) {
        memset(&workspace->relay_key, 0, sizeof(workspace->relay_key));
        workspace->relay_key.now_us = now_us;
        workspace->relay_key.realm = workspace->relay_fact.realm_id;
        workspace->relay_key.source_address =
            workspace->relay_fact.source_address;
        workspace->relay_key.source_binding_generation =
            workspace->relay_fact.source_binding_generation;
        workspace->relay_key.destination_address =
            workspace->relay_fact.destination_address;
        workspace->relay_key.destination_binding_generation =
            workspace->relay_fact.destination_binding_generation;
        result = ucn_i_route_forward_resolve(
            route_owner, &workspace->relay_key, &workspace->relay_route);
    }
    next_hop = result == UCN_OK ?
        composition_relay_next_hop(&workspace->relay_route) : NULL;
    if (result == UCN_OK &&
        (next_hop == NULL || next_hop->link_id == 0U ||
         next_hop->link_id ==
             (uint16_t)(workspace->control.rx.link_slot + 1U) ||
         next_hop->link_generation == 0U)) {
        result = UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        workspace->relay_link_slot = (uint16_t)(next_hop->link_id - 1U);
        workspace->relay_link_instance_generation =
            next_hop->link_generation;
        result = ucn_i_adapter_link_snapshot(
            &runtime->foundation_node->adapter,
            workspace->relay_link_slot, &egress_link_generation,
            &egress_mtu, &egress_ready);
    }
    if (result == UCN_OK &&
        (!egress_ready || egress_link_generation !=
             workspace->relay_link_instance_generation)) {
        result = UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        result = ucn_i_security_c0_hop_send_context_resolve(
            security_owner, workspace->relay_fact.protocol_opcode, now_us,
            workspace->relay_link_instance_generation,
            &workspace->relay_outbound_session,
            &workspace->relay_outbound_facts);
    }
    if (result == UCN_OK) {
        result = ucn_i_security_c0_hop_seal(
            security_owner, workspace->relay_outbound_session,
            &workspace->relay_outbound_facts, &workspace->relay_fact,
            &workspace->control.control.security.packet,
            workspace->control_body, sizeof(workspace->control_body),
            &workspace->relay_frame_bytes);
    }
    if (result == UCN_OK && workspace->relay_frame_bytes > egress_mtu) {
        result = UCN_ERR_NO_SPACE;
    }
    if (result == UCN_OK) {
        result = ucn_i_sha256_128(
            workspace->control_body, workspace->relay_frame_bytes,
            workspace->relay_frame_digest,
            &workspace->relay_digest_workspace);
    }
    if (result == UCN_OK) {
        result = ucn_i_adapter_tx_reserve(
            &runtime->foundation_node->adapter,
            workspace->relay_link_slot,
            workspace->relay_link_instance_generation, UINT16_MAX,
            &workspace->relay_tx_token);
        tx_reserved = result == UCN_OK;
    }
    if (result == UCN_OK) {
        result = ucn_i_adapter_tx_submit_ref(
            &runtime->foundation_node->adapter,
            &workspace->relay_tx_token, workspace->control_body,
            workspace->relay_frame_bytes, &workspace->relay_tx_view);
    }
    if (result == UCN_OK &&
        workspace->relay_tx_view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED) {
        result = UCN_ERR_TIMEOUT;
    }
    if (result == UCN_OK &&
        workspace->relay_tx_view.state == UCN_I_ADAPTER_TX_IN_DOUBT) {
        (void)ucn_i_security_hop_replay_commit(
            security_owner, &(workspace->relay_fact.replay),
            &workspace->relay_inbound_facts);
        composition_mark_fault(runtime);
        return UCN_ERR_IN_DOUBT;
    }
    if (result != UCN_OK) {
        if (tx_reserved) {
            if (workspace->relay_tx_view.state ==
                    UCN_I_ADAPTER_TX_NOT_SUBMITTED) {
                cleanup_result = ucn_i_adapter_tx_retire(
                    &runtime->foundation_node->adapter,
                    &(workspace->relay_tx_token));
            } else {
                cleanup_result = ucn_i_adapter_tx_cancel(
                    &runtime->foundation_node->adapter,
                    &(workspace->relay_tx_token));
                if (cleanup_result == UCN_OK) {
                    cleanup_result = ucn_i_adapter_tx_retire(
                        &runtime->foundation_node->adapter,
                        &(workspace->relay_tx_token));
                }
            }
            if (cleanup_result != UCN_OK) {
                composition_mark_fault(runtime);
                result = UCN_ERR_IN_DOUBT;
            }
        }
        if (replay_pending) {
            cleanup_result = ucn_i_security_hop_replay_abort(
                security_owner, &(workspace->relay_fact.replay));
            if (cleanup_result != UCN_OK) {
                composition_mark_fault(runtime);
                result = UCN_ERR_IN_DOUBT;
            }
        }
        cleanup_result = ucn_i_adapter_rx_retire(
            &runtime->foundation_node->adapter,
            &(workspace->control.rx.token));
        if (cleanup_result != UCN_OK) {
            composition_mark_fault(runtime);
            result = UCN_ERR_IN_DOUBT;
        }
        return result;
    }
    workspace->relay_tx_reserved = 1U;
    return UCN_OK;
}

static ucn_result_t composition_ingress_enter(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_ingress_workspace_t *workspace)
{
    ucn_i_callback_claim_t claim;
    ucn_result_t result = composition_static_c1_enter(
        runtime, UCN_I_COMPOSITION_OP_INGRESS_STEP, &claim);

    if (result != UCN_OK) {
        return result;
    }
    if (runtime->ingress_pending_kind != UCN_I_COMPOSITION_INGRESS_NONE) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    /* EN: Do not touch caller workspace until the gate and pending-state
     * preflight have both succeeded.  The claim then lives in caller-owned
     * scratch rather than on every nested Foundation call stack.
     * 中文：门禁与 pending 状态预检全部成功前不写调用方工作区；之后 claim
     * 存入调用方暂存，不再叠加到每条 Foundation 调用栈。 */
    memset(workspace, 0, sizeof(*workspace));
    workspace->scheduler_claim = claim;
    return UCN_OK;
}

static ucn_result_t composition_ingress_bootstrap_try(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_ingress_workspace_t *workspace)
{
    ucn_result_t result;

    if ((runtime->plan.capability_mask &
         UCN_I_CAP_DYNAMIC_ADMISSION) == 0U ||
        !composition_owner_is_active(
            runtime, UCN_I_COMPOSITION_MODULE_ADMISSION)) {
        return UCN_OK;
    }
    if (runtime->ingress_generation == UINT32_MAX) {
        composition_mark_fault(runtime);
        return UCN_ERR_EXHAUSTED;
    }
    result = composition_bootstrap_rx_claim_prepare_locked(
        runtime, &workspace->bootstrap);
    if (result == UCN_ERR_NOT_FOUND) {
        return UCN_OK;
    }
    if (result != UCN_OK) {
        return result;
    }
    workspace->bootstrap_plan = workspace->bootstrap.candidate;
    composition_ingress_pending_publish(
        runtime, workspace, UCN_I_COMPOSITION_INGRESS_BOOTSTRAP,
        &workspace->bootstrap_plan.rx_token);
    ++workspace->scheduler_operations;
    workspace->scheduler_selected_kind =
        UCN_I_COMPOSITION_INGRESS_BOOTSTRAP;
    return UCN_OK;
}

static ucn_result_t composition_ingress_control_try(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    ucn_i_composition_ingress_workspace_t *workspace)
{
    ucn_result_t result;

    if (!composition_owner_is_active(
            runtime, UCN_I_COMPOSITION_MODULE_SECURITY)) {
        return UCN_OK;
    }
    if (runtime->ingress_generation == UINT32_MAX) {
        composition_mark_fault(runtime);
        return UCN_ERR_EXHAUSTED;
    }
    memset(&workspace->control, 0, sizeof(workspace->control));
    result = ucn_i_adapter_rx_claim_contract_origin_range(
        &runtime->foundation_node->adapter,
        UCN_I_WIRE_PROTOCOL_VERSION, UCN_I_WIRE_CONTRACT_C0,
        UCN_I_ORIGIN_O1, UCN_I_ORIGIN_O2, &workspace->control.rx);
    if (result == UCN_ERR_NOT_FOUND) {
        return UCN_OK;
    }
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_security_c0_receive_context_resolve(
        owner_storage(runtime, UCN_I_COMPOSITION_MODULE_SECURITY),
        workspace->control.rx.frame, workspace->control.rx.frame_bytes,
        now_us, workspace->control.rx.link_instance_generation,
        &workspace->control.access, &workspace->control.facts);
    if (result == UCN_ERR_NOT_FOUND) {
        result = ucn_i_security_c0_origin_receive_context_resolve(
            owner_storage(runtime, UCN_I_COMPOSITION_MODULE_SECURITY),
            workspace->control.rx.frame,
            workspace->control.rx.frame_bytes, now_us,
            &workspace->control.origin_resolve,
            &workspace->control.access, &workspace->control.facts);
    }
    if (result == UCN_OK) {
        workspace->control.prepare.body_output = workspace->control_body;
        workspace->control.prepare.candidate_bytes_out =
            &workspace->control_body_bytes;
        workspace->control.prepare.body_capacity =
            sizeof(workspace->control_body);
        workspace->control.prepare.claim_rx = false;
        result = composition_control_rx_open_claimed_locked(
            runtime, now_us, &workspace->control);
        if (result == UCN_OK) {
            workspace->control_plan = workspace->control.candidate;
            composition_ingress_pending_publish(
                runtime, workspace,
                UCN_I_COMPOSITION_INGRESS_AUTHENTICATED_CONTROL,
                &workspace->control_plan.rx_token);
            workspace->scheduler_selected_kind =
                UCN_I_COMPOSITION_INGRESS_AUTHENTICATED_CONTROL;
        }
    } else if (result == UCN_ERR_NOT_FOUND) {
        /* EN: Keep the claimed O1/O2 item for the caller's relay phase.  The
         * relay performs Driver TX and must not inherit this resolver's stack
         * frame on Nano.
         * 中文：保留已认领的 O1/O2 条目给调用者执行 Relay；Relay 会进入
         * Driver TX，在 Nano 上不得叠加本解析器的栈帧。 */
        return UCN_ERR_NOT_FOUND;
    } else {
        ucn_result_t retire_result = ucn_i_adapter_rx_retire(
            &runtime->foundation_node->adapter,
            &(workspace->control.rx.token));

        if (retire_result != UCN_OK) {
            composition_mark_fault(runtime);
            result = UCN_ERR_IN_DOUBT;
        }
    }
    if (result == UCN_OK) {
        ++workspace->scheduler_operations;
    }
    return result;
}

ucn_result_t ucn_i_composition_ingress_step(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    const ucn_step_budget_t *budget,
    ucn_i_composition_ingress_workspace_t *workspace,
    ucn_i_composition_ingress_result_t *result_out)
{
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || now_us == 0U ||
        budget == NULL || workspace == NULL || result_out == NULL ||
        budget->struct_size != sizeof(*budget) ||
        budget->api_version != UCN_API_VERSION || budget->max_work == 0U ||
        budget->reserved_zero != 0U ||
        !composition_external_range(runtime, budget, sizeof(*budget)) ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        !composition_external_range(runtime, result_out,
                                    sizeof(*result_out)) ||
        ucn_i_ranges_overlap(budget, sizeof(*budget), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(budget, sizeof(*budget), result_out,
                             sizeof(*result_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), result_out,
                             sizeof(*result_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_ingress_enter(runtime, workspace);
    if (result != UCN_OK) {
        return result;
    }
    workspace->foundation_budget.struct_size =
        (uint16_t)sizeof(workspace->foundation_budget);
    workspace->foundation_budget.api_version = UCN_API_VERSION;
    workspace->foundation_budget.max_work = 1U;

    while (workspace->scheduler_inspected <
               UCN_I_COMPOSITION_INGRESS_CLASS_COUNT &&
           workspace->scheduler_operations < budget->max_work) {
        const uint8_t selected = runtime->ingress_cursor;

        runtime->ingress_cursor = (uint8_t)(
            (runtime->ingress_cursor + 1U) %
            UCN_I_COMPOSITION_INGRESS_CLASS_COUNT);
        ++workspace->scheduler_inspected;
        if (selected == 0U) {
            memset(&workspace->foundation_result, 0,
                   sizeof(workspace->foundation_result));
            result = ucn_step(runtime->foundation_node, now_us,
                              &workspace->foundation_budget,
                              &workspace->foundation_result);
            if (result != UCN_OK) {
                break;
            }
            workspace->scheduler_foundation_more_work =
                workspace->foundation_result.more_work != 0U ? 1U : 0U;
            if (workspace->foundation_result.work_done != 0U) {
                ++workspace->scheduler_operations;
                workspace->scheduler_selected_kind =
                    UCN_I_COMPOSITION_INGRESS_FOUNDATION;
            }
            continue;
        }
        if (selected == 1U) {
            result = composition_ingress_bootstrap_try(runtime, workspace);
        } else {
            result = composition_ingress_control_try(
                runtime, now_us, workspace);
            if (result == UCN_ERR_NOT_FOUND) {
                result = composition_control_relay_prepare_claimed_locked(
                    runtime, now_us, workspace);
                if (result == UCN_OK) {
                    composition_ingress_pending_publish(
                        runtime, workspace,
                        UCN_I_COMPOSITION_INGRESS_CONTROL_RELAY,
                        &workspace->control.rx.token);
                    workspace->scheduler_selected_kind =
                        UCN_I_COMPOSITION_INGRESS_CONTROL_RELAY;
                    ++workspace->scheduler_operations;
                }
            }
        }
        if (result != UCN_OK) {
            break;
        }
        if (workspace->scheduler_selected_kind !=
            UCN_I_COMPOSITION_INGRESS_NONE) {
            break;
        }
    }
    if (result == UCN_OK) {
        const bool more_work =
            runtime->ingress_pending_kind !=
                UCN_I_COMPOSITION_INGRESS_NONE ||
            workspace->scheduler_foundation_more_work != 0U ||
            ucn_i_adapter_has_work(&runtime->foundation_node->adapter);

        composition_ingress_result_fill(
            runtime, workspace->scheduler_operations,
            workspace->scheduler_selected_kind, more_work,
            &workspace->result_candidate);
    }
    result = composition_leave(runtime, &workspace->scheduler_claim, result);
    memset(&workspace->scheduler_claim, 0,
           sizeof(workspace->scheduler_claim));
    workspace->scheduler_operations = 0U;
    workspace->scheduler_inspected = 0U;
    workspace->scheduler_selected_kind = 0U;
    workspace->scheduler_foundation_more_work = 0U;
    memset(workspace->scheduler_reserved, 0,
           sizeof(workspace->scheduler_reserved));
    if (result == UCN_OK) {
        *result_out = workspace->result_candidate;
    } else if (runtime->ingress_pending_kind !=
               UCN_I_COMPOSITION_INGRESS_NONE) {
        ucn_i_security_owner_t *security_owner = owner_storage(
            runtime, UCN_I_COMPOSITION_MODULE_SECURITY);

        if (runtime->ingress_pending_kind ==
                UCN_I_COMPOSITION_INGRESS_AUTHENTICATED_CONTROL &&
            security_owner != NULL) {
            (void)ucn_i_security_replay_abort(
                security_owner,
                &(workspace->control_plan.dispatch.authenticated.replay));
            if (workspace->control_plan.has_hop_replay != 0U) {
                (void)ucn_i_security_hop_replay_abort(
                    security_owner, &(workspace->control_plan.hop_replay));
            }
        } else if (runtime->ingress_pending_kind ==
                       UCN_I_COMPOSITION_INGRESS_CONTROL_RELAY &&
                   security_owner != NULL) {
            (void)ucn_i_security_hop_replay_abort(
                security_owner, &(workspace->relay_fact.replay));
            if (workspace->relay_tx_reserved != 0U) {
                (void)ucn_i_adapter_tx_cancel(
                    &runtime->foundation_node->adapter,
                    &(workspace->relay_tx_token));
                (void)ucn_i_adapter_tx_retire(
                    &runtime->foundation_node->adapter,
                    &(workspace->relay_tx_token));
            }
        }
        (void)ucn_i_adapter_rx_retire(
            &runtime->foundation_node->adapter,
            &(runtime->ingress_pending_token));
        runtime->ingress_pending_kind = UCN_I_COMPOSITION_INGRESS_NONE;
        memset(&runtime->ingress_pending_token, 0,
               sizeof(runtime->ingress_pending_token));
        memset(workspace, 0, sizeof(*workspace));
    }
    return result;
}

static bool composition_ingress_workspace_matches_pending(
    const ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_ingress_workspace_t *workspace)
{
    ucn_driver_token_t token;

    if (workspace->struct_size != sizeof(*workspace) ||
        workspace->schema != UCN_I_COMPOSITION_INGRESS_SCHEMA ||
        workspace->runtime_instance != runtime->runtime_instance ||
        workspace->generation != runtime->ingress_generation ||
        workspace->active_kind != runtime->ingress_pending_kind ||
        !bytes_are_zero(workspace->reserved_zero,
                        sizeof(workspace->reserved_zero))) {
        return false;
    }
    if (workspace->active_kind == UCN_I_COMPOSITION_INGRESS_BOOTSTRAP) {
        token = workspace->bootstrap_plan.rx_token;
    } else if (workspace->active_kind ==
               UCN_I_COMPOSITION_INGRESS_AUTHENTICATED_CONTROL) {
        if (workspace->control_plan.dispatch.authenticated.body !=
                workspace->control_body ||
            workspace->control_plan.dispatch.authenticated.body_bytes !=
                workspace->control_body_bytes) {
            return false;
        }
        token = workspace->control_plan.rx_token;
    } else if (workspace->active_kind ==
               UCN_I_COMPOSITION_INGRESS_CONTROL_RELAY) {
        if (workspace->relay_tx_reserved == 0U ||
            workspace->reserved_relay != 0U ||
            workspace->relay_frame_bytes == 0U ||
            workspace->relay_frame_bytes > UCN_ADAPTER_FRAME_BYTES ||
            workspace->relay_fact.packet != workspace->control.rx.frame ||
            workspace->relay_fact.packet_bytes !=
                workspace->control.rx.frame_bytes) {
            return false;
        }
        token = workspace->control.rx.token;
    } else {
        return false;
    }
    return composition_token_equal(token, runtime->ingress_pending_token);
}

static ucn_result_t composition_control_relay_finish_locked(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_ingress_workspace_t *workspace,
    bool *terminal_out)
{
    ucn_i_security_owner_t *security_owner;
    ucn_i_adapter_tx_view_t view;
    uint8_t digest[UCN_I_DIGEST128_BYTES];
    bool commit_replay = false;
    bool abort_replay = false;
    bool retire_tx = false;
    ucn_result_t result;
    ucn_result_t replay_result;
    ucn_result_t retire_result;

    *terminal_out = false;
    security_owner = owner_storage(
        runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    if (security_owner == NULL) {
        return UCN_ERR_STATE;
    }
    result = ucn_i_sha256_128(
        workspace->control_body, workspace->relay_frame_bytes, digest,
        &workspace->relay_digest_workspace);
    if (result != UCN_OK ||
        memcmp(digest, workspace->relay_frame_digest,
               sizeof(digest)) != 0) {
        result = ucn_i_adapter_tx_cancel(
            &runtime->foundation_node->adapter,
            &(workspace->relay_tx_token));
        if (result == UCN_OK) {
            abort_replay = true;
            retire_tx = true;
            result = UCN_ERR_SECURITY;
        } else {
            commit_replay = true;
            composition_mark_fault(runtime);
            result = UCN_ERR_IN_DOUBT;
        }
        *terminal_out = true;
    } else {
        result = ucn_i_adapter_tx_view(
            &runtime->foundation_node->adapter,
            &(workspace->relay_tx_token), &view);
        if (result != UCN_OK) {
            return result;
        }
        workspace->relay_tx_view = view;
        switch (view.state) {
            case UCN_I_ADAPTER_TX_SUBMITTING:
            case UCN_I_ADAPTER_TX_SUBMITTED:
                return UCN_ERR_STATE;
            case UCN_I_ADAPTER_TX_COMPLETED:
                commit_replay = true;
                retire_tx = true;
                result = view.terminal_result;
                *terminal_out = true;
                break;
            case UCN_I_ADAPTER_TX_NOT_SUBMITTED:
            case UCN_I_ADAPTER_TX_CANCELLED:
                abort_replay = true;
                retire_tx = true;
                result = view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED ?
                    UCN_ERR_TIMEOUT : UCN_ERR_CANCELLED;
                *terminal_out = true;
                break;
            case UCN_I_ADAPTER_TX_IN_DOUBT:
                commit_replay = true;
                composition_mark_fault(runtime);
                result = UCN_ERR_IN_DOUBT;
                *terminal_out = true;
                break;
            default:
                return UCN_ERR_STATE;
        }
    }
    replay_result = commit_replay ?
        ucn_i_security_hop_replay_commit(
            security_owner, &(workspace->relay_fact.replay),
            &workspace->relay_inbound_facts) :
        (abort_replay ? ucn_i_security_hop_replay_abort(
             security_owner, &(workspace->relay_fact.replay)) : UCN_OK);
    if (replay_result != UCN_OK) {
        composition_mark_fault(runtime);
        result = UCN_ERR_IN_DOUBT;
    }
    if (retire_tx) {
        retire_result = ucn_i_adapter_tx_retire(
            &runtime->foundation_node->adapter,
            &(workspace->relay_tx_token));
        if (retire_result != UCN_OK) {
            composition_mark_fault(runtime);
            result = UCN_ERR_IN_DOUBT;
        }
    }
    retire_result = ucn_i_adapter_rx_retire(
        &runtime->foundation_node->adapter,
        &(workspace->control.rx.token));
    if (retire_result != UCN_OK) {
        composition_mark_fault(runtime);
        result = UCN_ERR_IN_DOUBT;
    }
    return result;
}

ucn_result_t ucn_i_composition_ingress_finish(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_ingress_workspace_t *workspace,
    bool consumed)
{
    ucn_i_callback_claim_t claim;
    ucn_i_composition_module_id_t target_module =
        UCN_I_COMPOSITION_MODULE_COORDINATOR;
    uint8_t opcode_domain = 0U;
    bool terminal = true;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || workspace == NULL ||
        !composition_external_range(runtime, workspace, sizeof(*workspace)) ||
        !composition_ingress_workspace_matches_pending(runtime, workspace)) {
        return UCN_ERR_ARGUMENT;
    }
    if (workspace->active_kind ==
        UCN_I_COMPOSITION_INGRESS_AUTHENTICATED_CONTROL) {
        result = control_target_module(
            workspace->control_plan.dispatch.authenticated.protocol_opcode,
            &target_module, &opcode_domain);
        if (result != UCN_OK ||
            workspace->control_plan.dispatch.target_module !=
                (uint8_t)target_module ||
            workspace->control_plan.dispatch.opcode_domain != opcode_domain) {
            return result == UCN_OK ? UCN_ERR_STATE : result;
        }
        result = composition_control_enter(
            runtime, target_module, UCN_I_COMPOSITION_OP_INGRESS_FINISH,
            &claim);
    } else if (workspace->active_kind ==
               UCN_I_COMPOSITION_INGRESS_BOOTSTRAP) {
        result = composition_bootstrap_enter(
            runtime, UCN_I_COMPOSITION_OP_INGRESS_FINISH, &claim);
    } else {
        result = composition_control_enter(
            runtime, UCN_I_COMPOSITION_MODULE_COORDINATOR,
            UCN_I_COMPOSITION_OP_INGRESS_FINISH, &claim);
    }
    if (result != UCN_OK) {
        return result;
    }
    if (workspace->active_kind == UCN_I_COMPOSITION_INGRESS_BOOTSTRAP) {
        result = composition_bootstrap_rx_finish_locked(
            runtime, &workspace->bootstrap_plan,
            &workspace->bootstrap.digest_workspace, consumed);
    } else if (workspace->active_kind ==
               UCN_I_COMPOSITION_INGRESS_AUTHENTICATED_CONTROL) {
        result = composition_control_rx_finish_locked(
            runtime, &workspace->control_plan,
            &workspace->control.control.validate, consumed);
    } else {
        result = composition_control_relay_finish_locked(
            runtime, workspace, &terminal);
    }
    if (!terminal) {
        return composition_leave(runtime, &claim, result);
    }
    runtime->ingress_pending_kind = UCN_I_COMPOSITION_INGRESS_NONE;
    memset(&runtime->ingress_pending_token, 0,
           sizeof(runtime->ingress_pending_token));
    memset(workspace, 0, sizeof(*workspace));
    return composition_leave(runtime, &claim, result);
}

ucn_result_t ucn_i_composition_advanced_use_finish(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_advanced_use_plan_t *plan,
    uint8_t delivered)
{
    ucn_i_flow_owner_t *flow_owner;
    ucn_i_callback_claim_t claim;
    ucn_i_flow_phase_t phase;
    ucn_result_t result;

    if (runtime == NULL || plan == NULL || delivered > 1U ||
        plan->struct_size != sizeof(*plan) ||
        plan->schema != UCN_I_COMPOSITION_ADVANCED_USE_SCHEMA ||
        plan->runtime_instance != runtime->runtime_instance ||
        plan->route_kind != UCN_I_ROUTE_RESOLVED_DYNAMIC ||
        !bytes_are_zero(plan->reserved_zero,
                        sizeof(plan->reserved_zero)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             plan, sizeof(*plan))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_advanced_enter(
        runtime, UCN_I_COMPOSITION_OP_ADVANCED_FINISH, &claim);
    if (result != UCN_OK) {
        return result;
    }
    flow_owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_FLOW);
    if (flow_owner == NULL) {
        return composition_leave(runtime, &claim, UCN_ERR_STATE);
    }
    if (plan->flow_plan.sequence == 0U) {
        result = ucn_i_flow_phase(flow_owner, plan->flow, &phase);
        if (result == UCN_OK && phase != UCN_I_FLOW_ACTIVE) {
            result = UCN_ERR_STATE;
        }
    } else if (delivered != 0U) {
        result = ucn_i_flow_sequence_commit(
            flow_owner, plan->flow, plan->flow_plan.sequence,
            plan->flow_plan.reservation_generation);
    } else {
        result = ucn_i_flow_sequence_abort(
            flow_owner, plan->flow, plan->flow_plan.sequence,
            plan->flow_plan.reservation_generation);
    }
    return composition_leave(runtime, &claim, result);
}

#if UCN_V6S_FEATURE_REALTIME_ENABLED
ucn_result_t ucn_i_composition_realtime_local_policy_set(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_realtime_policy_t *policy)
{
    ucn_i_realtime_owner_t *owner;
    ucn_i_callback_claim_t claim;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || policy == NULL ||
        policy->mode != UCN_I_REALTIME_LOCAL_STAMP ||
        !composition_external_range(runtime, policy, sizeof(*policy))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_optional_enter(
        runtime, UCN_I_COMPOSITION_MODULE_REALTIME,
        UCN_I_COMPOSE_REALTIME, UCN_I_CAP_LOCAL_STAMP,
        UCN_I_COMPOSITION_OP_REALTIME_POLICY, &claim);
    if (result != UCN_OK) {
        return result;
    }
    owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_REALTIME);
    result = owner == NULL ? UCN_ERR_STATE
                           : ucn_i_realtime_policy_set(owner, policy);
    return composition_leave(runtime, &claim, result);
}

ucn_result_t ucn_i_composition_realtime_local_prepare(
    ucn_i_composition_runtime_t *runtime, uint16_t endpoint,
    uint64_t local_capture_us, uint32_t sample_capture_bound_us,
    bool hardware_capture, ucn_i_realtime_prepared_t *prepared_out)
{
    ucn_i_realtime_owner_t *owner;
    ucn_i_realtime_prepared_t prepared = {0};
    ucn_i_callback_claim_t claim;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) ||
        !composition_external_range(runtime, prepared_out,
                                    sizeof(*prepared_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_optional_enter(
        runtime, UCN_I_COMPOSITION_MODULE_REALTIME,
        UCN_I_COMPOSE_REALTIME, UCN_I_CAP_LOCAL_STAMP,
        UCN_I_COMPOSITION_OP_REALTIME_PREPARE, &claim);
    if (result != UCN_OK) {
        return result;
    }
    owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_REALTIME);
    result = owner == NULL
                 ? UCN_ERR_STATE
                 : ucn_i_realtime_prepare(
                       owner, endpoint, local_capture_us,
                       sample_capture_bound_us, hardware_capture, &prepared);
    if (result == UCN_OK &&
        (prepared.metadata_present == 0U ||
         prepared.envelope.mode != UCN_I_REALTIME_LOCAL_STAMP ||
         prepared.envelope.domain_time_valid != 0U ||
         prepared.envelope.domain_generation != 0U ||
         prepared.envelope.clock_domain_id != 0U)) {
        result = UCN_ERR_STATE;
    }
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *prepared_out = prepared;
    }
    return result;
}
#endif

#if UCN_V6S_FEATURE_GROUP_ENABLED
ucn_result_t ucn_i_composition_group_install_static(
    ucn_i_composition_runtime_t *runtime, uint16_t static_slot,
    const ucn_i_group_context_config_t *config,
    bool manifest_authenticated, bool manifest_anti_rollback,
    ucn_handle_t *group_out)
{
    ucn_i_group_owner_t *owner;
    ucn_i_callback_claim_t claim;
    ucn_handle_t group = {0};
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || config == NULL ||
        config->mode != UCN_I_GROUP_STATIC ||
        !composition_external_range(runtime, config, sizeof(*config)) ||
        !composition_external_range(runtime, group_out,
                                    sizeof(*group_out)) ||
        ucn_i_ranges_overlap(config, sizeof(*config), group_out,
                             sizeof(*group_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_optional_enter(
        runtime, UCN_I_COMPOSITION_MODULE_GROUP, UCN_I_COMPOSE_GROUP,
        UCN_I_CAP_STATIC_GROUP,
        UCN_I_COMPOSITION_OP_GROUP_INSTALL_STATIC, &claim);
    if (result != UCN_OK) {
        return result;
    }
    owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_GROUP);
    result = owner == NULL
                 ? UCN_ERR_STATE
                 : ucn_i_group_install_static(
                       owner, static_slot, config, manifest_authenticated,
                       manifest_anti_rollback, &group);
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *group_out = group;
    }
    return result;
}

ucn_result_t ucn_i_composition_group_send_begin(
    ucn_i_composition_runtime_t *runtime, ucn_handle_t group,
    const ucn_i_group_send_request_t *request, uint64_t now_us,
    ucn_handle_t *send_out)
{
    ucn_i_group_owner_t *owner;
    ucn_i_callback_claim_t claim;
    ucn_handle_t send = {0};
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || request == NULL ||
        !composition_external_range(runtime, request, sizeof(*request)) ||
        !composition_external_range(runtime, send_out, sizeof(*send_out)) ||
        ucn_i_ranges_overlap(request, sizeof(*request), send_out,
                             sizeof(*send_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_optional_enter(
        runtime, UCN_I_COMPOSITION_MODULE_GROUP, UCN_I_COMPOSE_GROUP,
        UCN_I_CAP_STATIC_GROUP, UCN_I_COMPOSITION_OP_GROUP_SEND_BEGIN,
        &claim);
    if (result != UCN_OK) {
        return result;
    }
    owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_GROUP);
    result = owner == NULL
                 ? UCN_ERR_STATE
                 : ucn_i_group_send_begin(owner, group, request, now_us,
                                          &send);
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *send_out = send;
    }
    return result;
}

ucn_result_t ucn_i_composition_group_attempt_peek(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_group_attempt_view_t *attempt_out)
{
    ucn_i_group_owner_t *owner;
    ucn_i_group_attempt_view_t attempt = {0};
    ucn_i_callback_claim_t claim;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) ||
        !composition_external_range(runtime, attempt_out,
                                    sizeof(*attempt_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_optional_enter(
        runtime, UCN_I_COMPOSITION_MODULE_GROUP, UCN_I_COMPOSE_GROUP,
        UCN_I_CAP_STATIC_GROUP, UCN_I_COMPOSITION_OP_GROUP_ATTEMPT_PEEK,
        &claim);
    if (result != UCN_OK) {
        return result;
    }
    owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_GROUP);
    result = owner == NULL ? UCN_ERR_STATE
                           : ucn_i_group_attempt_peek(owner, &attempt);
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *attempt_out = attempt;
    }
    return result;
}
#endif

#if UCN_V6S_FEATURE_CLUSTER_ENABLED
ucn_result_t ucn_i_composition_cluster_member_observe(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_cluster_member_fact_t *fact, uint64_t now_us)
{
    ucn_i_cluster_owner_t *owner;
    ucn_i_callback_claim_t claim;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || fact == NULL ||
        !composition_external_range(runtime, fact, sizeof(*fact))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_optional_enter(
        runtime, UCN_I_COMPOSITION_MODULE_CLUSTER,
        UCN_I_COMPOSE_CLUSTER, UCN_I_CAP_CLUSTER,
        UCN_I_COMPOSITION_OP_CLUSTER_MEMBER_OBSERVE, &claim);
    if (result != UCN_OK) {
        return result;
    }
    owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_CLUSTER);
    result = owner == NULL ? UCN_ERR_STATE
                           : ucn_i_cluster_member_observe(owner, fact,
                                                          now_us);
    return composition_leave(runtime, &claim, result);
}

ucn_result_t ucn_i_composition_cluster_authority_preflight(
    ucn_i_composition_runtime_t *runtime, uint64_t now_us,
    ucn_i_cluster_authority_view_t *view_out)
{
    ucn_i_cluster_owner_t *owner;
    ucn_i_cluster_authority_view_t view = {0};
    ucn_i_callback_claim_t claim;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) ||
        !composition_external_range(runtime, view_out,
                                    sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_optional_enter(
        runtime, UCN_I_COMPOSITION_MODULE_CLUSTER,
        UCN_I_COMPOSE_CLUSTER, UCN_I_CAP_CLUSTER,
        UCN_I_COMPOSITION_OP_CLUSTER_AUTHORITY_PREFLIGHT, &claim);
    if (result != UCN_OK) {
        return result;
    }
    owner = owner_storage(runtime, UCN_I_COMPOSITION_MODULE_CLUSTER);
    result = owner == NULL
                 ? UCN_ERR_STATE
                 : ucn_i_cluster_authority_preflight(owner, now_us, &view);
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *view_out = view;
    }
    return result;
}
#endif

static void composition_mark_fault(
    ucn_i_composition_runtime_t *runtime)
{
    uint8_t index;

    runtime->registry[0].state =
        UCN_I_COMPOSITION_OWNER_INITIALIZED;
    for (index = 0U; index < runtime->initialized_count; ++index) {
        ucn_i_composition_owner_record_t *record =
            owner_record_find_mutable(runtime,
                                      runtime->plan.init_order[index]);
        if (record != NULL) {
            record->state = UCN_I_COMPOSITION_OWNER_INITIALIZED;
        }
    }
    runtime->phase = UCN_I_COMPOSITION_FAULT;
}

ucn_result_t ucn_i_composition_endpoint_add(
    ucn_i_composition_runtime_t *runtime,
    const ucn_endpoint_config_t *config,
    ucn_endpoint_handle_t *endpoint_out)
{
    ucn_i_callback_claim_t claim;
    ucn_endpoint_handle_t endpoint;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || config == NULL ||
        !composition_external_range(runtime, config, sizeof(*config)) ||
        !composition_external_range(runtime, endpoint_out,
                                    sizeof(*endpoint_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_static_c1_enter(
        runtime, UCN_I_COMPOSITION_OP_ENDPOINT_ADD, &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_endpoint_add(runtime->foundation_node, config, &endpoint);
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *endpoint_out = endpoint;
    }
    return result;
}

ucn_result_t ucn_i_composition_endpoint_remove(
    ucn_i_composition_runtime_t *runtime,
    ucn_endpoint_handle_t endpoint)
{
    ucn_i_callback_claim_t claim;
    ucn_result_t result = composition_static_c1_enter(
        runtime, UCN_I_COMPOSITION_OP_ENDPOINT_REMOVE, &claim);

    if (result != UCN_OK) {
        return result;
    }
    result = ucn_endpoint_remove(runtime->foundation_node, endpoint);
    return composition_leave(runtime, &claim, result);
}

ucn_result_t ucn_i_composition_static_path_add(
    ucn_i_composition_runtime_t *runtime,
    const ucn_static_path_t *path,
    ucn_path_handle_t *path_out)
{
    ucn_i_callback_claim_t claim;
    ucn_path_handle_t result_path;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || path == NULL ||
        !composition_external_range(runtime, path, sizeof(*path)) ||
        !composition_external_range(runtime, path_out,
                                    sizeof(*path_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_static_c1_enter(
        runtime, UCN_I_COMPOSITION_OP_PATH_ADD, &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_static_path_add(runtime->foundation_node, path,
                                 &result_path);
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *path_out = result_path;
    }
    return result;
}

ucn_result_t ucn_i_composition_static_path_remove(
    ucn_i_composition_runtime_t *runtime,
    ucn_path_handle_t path)
{
    ucn_i_callback_claim_t claim;
    ucn_result_t result = composition_static_c1_enter(
        runtime, UCN_I_COMPOSITION_OP_PATH_REMOVE, &claim);

    if (result != UCN_OK) {
        return result;
    }
    result = ucn_static_path_remove(runtime->foundation_node, path);
    return composition_leave(runtime, &claim, result);
}

ucn_result_t ucn_i_composition_link_get(
    ucn_i_composition_runtime_t *runtime,
    uint16_t link_index,
    ucn_link_handle_t *link_out)
{
    ucn_i_callback_claim_t claim;
    ucn_link_handle_t link;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) ||
        !composition_external_range(runtime, link_out, sizeof(*link_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_static_c1_enter(
        runtime, UCN_I_COMPOSITION_OP_LINK_GET, &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_link_get(runtime->foundation_node, link_index, &link);
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *link_out = link;
    }
    return result;
}

ucn_result_t ucn_i_composition_publish(
    ucn_i_composition_runtime_t *runtime,
    const ucn_target_t *target,
    const void *payload,
    size_t payload_bytes,
    const ucn_send_options_t *options,
    ucn_send_handle_t *handle_out)
{
    ucn_i_callback_claim_t claim;
    ucn_send_handle_t handle = {0};
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || target == NULL ||
        options == NULL ||
        !composition_external_range(runtime, target, sizeof(*target)) ||
        !composition_external_range(runtime, options, sizeof(*options)) ||
        (payload_bytes != 0U &&
         !composition_external_range(runtime, payload, payload_bytes)) ||
        (handle_out != NULL &&
         !composition_external_range(runtime, handle_out,
                                     sizeof(*handle_out)))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_static_c1_enter(
        runtime, UCN_I_COMPOSITION_OP_PUBLISH, &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_send(runtime->foundation_node, target, payload,
                         payload_bytes, options,
                         handle_out == NULL ? NULL : &handle);
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK && handle_out != NULL) {
        *handle_out = handle;
    }
    return result;
}

ucn_result_t ucn_i_composition_send_query(
    ucn_i_composition_runtime_t *runtime,
    ucn_send_handle_t handle,
    ucn_send_view_t *view_out)
{
    ucn_i_callback_claim_t claim;
    ucn_send_view_t view;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) ||
        !composition_external_range(runtime, view_out, sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_static_c1_enter(
        runtime, UCN_I_COMPOSITION_OP_SEND_QUERY, &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_send_query(runtime->foundation_node, handle, &view);
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *view_out = view;
    }
    return result;
}

ucn_result_t ucn_i_composition_send_cancel(
    ucn_i_composition_runtime_t *runtime,
    ucn_send_handle_t handle)
{
    ucn_i_callback_claim_t claim;
    ucn_result_t result = composition_static_c1_enter(
        runtime, UCN_I_COMPOSITION_OP_SEND_CANCEL, &claim);

    if (result != UCN_OK) {
        return result;
    }
    result = ucn_send_cancel(runtime->foundation_node, handle);
    return composition_leave(runtime, &claim, result);
}

ucn_result_t ucn_i_composition_send_forget(
    ucn_i_composition_runtime_t *runtime,
    ucn_send_handle_t handle)
{
    ucn_i_callback_claim_t claim;
    ucn_result_t result = composition_static_c1_enter(
        runtime, UCN_I_COMPOSITION_OP_SEND_FORGET, &claim);

    if (result != UCN_OK) {
        return result;
    }
    result = ucn_send_forget(runtime->foundation_node, handle);
    return composition_leave(runtime, &claim, result);
}

ucn_result_t ucn_i_composition_get_stats(
    ucn_i_composition_runtime_t *runtime,
    ucn_stats_t *stats_out)
{
    ucn_i_callback_claim_t claim;
    ucn_stats_t stats;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) ||
        !composition_external_range(runtime, stats_out,
                                    sizeof(*stats_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_static_c1_enter(
        runtime, UCN_I_COMPOSITION_OP_STATS, &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_get_stats(runtime->foundation_node, &stats);
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *stats_out = stats;
    }
    return result;
}

ucn_result_t ucn_i_composition_step(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    const ucn_step_budget_t *budget,
    ucn_step_result_t *result_out)
{
    ucn_i_callback_claim_t claim;
    ucn_step_result_t step_result;
    ucn_result_t result;

    if (!composition_runtime_header_valid(runtime) || budget == NULL ||
        !composition_external_range(runtime, budget, sizeof(*budget)) ||
        !composition_external_range(runtime, result_out,
                                    sizeof(*result_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_static_c1_enter(
        runtime, UCN_I_COMPOSITION_OP_RUN_STEP, &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_step(runtime->foundation_node, now_us, budget,
                      &step_result);
    if (runtime->foundation_node->faulted != 0U ||
        runtime->foundation_node->lifecycle == UCN_LIFECYCLE_FAULT) {
        composition_mark_fault(runtime);
    }
    result = composition_leave(runtime, &claim, result);
    if (result == UCN_OK) {
        *result_out = step_result;
    }
    return result;
}

static bool composition_callback_input_is_valid(
    const ucn_i_composition_runtime_t *runtime,
    ucn_callback_scope_t scope,
    const void *output,
    size_t output_bytes)
{
    return composition_fast_path_header_valid(runtime) &&
           runtime->phase == UCN_I_COMPOSITION_RUNNING &&
           scope.runtime_instance == runtime->registry[0].ref.runtime_instance &&
           scope.owner_instance == runtime->registry[0].ref.owner_instance &&
           scope.reserved_zero == 0U && scope.nonce != 0U &&
           composition_external_range(runtime, output, output_bytes);
}

ucn_result_t ucn_i_composition_callback_link_get(
    const ucn_i_composition_runtime_t *runtime,
    ucn_callback_scope_t scope,
    uint16_t link_index,
    ucn_link_handle_t *link_out)
{
    if (!composition_callback_input_is_valid(
            runtime, scope, link_out, sizeof(*link_out))) {
        return UCN_ERR_ARGUMENT;
    }
    return ucn_callback_link_get(runtime->foundation_node, scope,
                                 link_index, link_out);
}

ucn_result_t ucn_i_composition_callback_send_query(
    const ucn_i_composition_runtime_t *runtime,
    ucn_callback_scope_t scope,
    ucn_send_handle_t handle,
    ucn_send_view_t *view_out)
{
    if (!composition_callback_input_is_valid(
            runtime, scope, view_out, sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    return ucn_callback_send_query(runtime->foundation_node, scope,
                                   handle, view_out);
}

ucn_result_t ucn_i_composition_callback_get_stats(
    const ucn_i_composition_runtime_t *runtime,
    ucn_callback_scope_t scope,
    ucn_stats_t *stats_out)
{
    if (!composition_callback_input_is_valid(
            runtime, scope, stats_out, sizeof(*stats_out))) {
        return UCN_ERR_ARGUMENT;
    }
    return ucn_callback_get_stats(runtime->foundation_node, scope,
                                  stats_out);
}

static ucn_result_t composition_driver_enter(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_callback_claim_t *claim_out,
    bool *nested_out)
{
    bool active = false;
    ucn_i_callback_claim_t active_claim;

    if (!composition_fast_path_header_valid(runtime) ||
        claim_out == NULL || nested_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (runtime->phase != UCN_I_COMPOSITION_RUNNING &&
        runtime->phase != UCN_I_COMPOSITION_STOPPING) {
        return UCN_ERR_STATE;
    }
    if (ucn_i_callback_gate_view(&runtime->lifecycle_gate, &active,
                                 &active_claim) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (active) {
        if (!composition_gate_claim_is_exact(
                runtime, &active_claim,
                UCN_I_COMPOSITION_OP_RUN_STEP) &&
            !composition_gate_claim_is_exact(
                runtime, &active_claim,
                UCN_I_COMPOSITION_OP_STOP_STEP)) {
            return UCN_ERR_STATE;
        }
        *nested_out = true;
        memset(claim_out, 0, sizeof(*claim_out));
        return UCN_OK;
    }
    if (!ucn_i_composition_runtime_valid(runtime)) {
        return UCN_ERR_ARGUMENT;
    }
    *nested_out = false;
    return composition_enter(runtime,
                             UCN_I_COMPOSITION_OP_DRIVER_INGRESS,
                             claim_out);
}

static ucn_result_t composition_driver_leave(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_callback_claim_t *claim,
    bool nested,
    ucn_result_t result)
{
    return nested ? result : composition_leave(runtime, claim, result);
}

ucn_result_t ucn_i_composition_driver_rx_publish(
    ucn_i_composition_runtime_t *runtime,
    ucn_link_handle_t link,
    const uint8_t *bytes,
    size_t length,
    const ucn_rx_meta_t *meta)
{
    ucn_i_callback_claim_t claim;
    bool nested;
    ucn_result_t result;

    if (!composition_fast_path_header_valid(runtime) || bytes == NULL ||
        meta == NULL || length == 0U ||
        !composition_external_range(runtime, bytes, length) ||
        !composition_external_range(runtime, meta, sizeof(*meta))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_driver_enter(runtime, &claim, &nested);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_driver_rx_publish(runtime->foundation_node, link, bytes,
                                   length, meta);
    return composition_driver_leave(runtime, &claim, nested, result);
}

ucn_result_t ucn_i_composition_driver_tx_complete(
    ucn_i_composition_runtime_t *runtime,
    ucn_driver_token_t token,
    ucn_result_t terminal_result,
    const ucn_tx_meta_t *meta)
{
    ucn_i_callback_claim_t claim;
    bool nested;
    ucn_result_t result;

    if (!composition_fast_path_header_valid(runtime) ||
        (meta != NULL &&
         !composition_external_range(runtime, meta, sizeof(*meta)))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_driver_enter(runtime, &claim, &nested);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_driver_tx_complete(runtime->foundation_node, token,
                                    terminal_result, meta);
    return composition_driver_leave(runtime, &claim, nested, result);
}

ucn_result_t ucn_i_composition_driver_link_event(
    ucn_i_composition_runtime_t *runtime,
    ucn_link_handle_t link,
    ucn_driver_link_event_t event,
    const ucn_link_event_meta_t *meta)
{
    ucn_i_callback_claim_t claim;
    bool nested;
    ucn_result_t result;

    if (!composition_fast_path_header_valid(runtime) ||
        (meta != NULL &&
         !composition_external_range(runtime, meta, sizeof(*meta)))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_driver_enter(runtime, &claim, &nested);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_driver_link_event(runtime->foundation_node, link, event,
                                   meta);
    return composition_driver_leave(runtime, &claim, nested, result);
}

ucn_result_t ucn_i_composition_destroy(
    ucn_i_composition_runtime_t *runtime)
{
    const uint32_t storage_bytes =
        runtime == NULL ? 0U : runtime->storage_bytes;
    ucn_result_t result;

    if (!ucn_i_composition_runtime_valid(runtime) ||
        (runtime->phase != UCN_I_COMPOSITION_PREPARED &&
         runtime->phase != UCN_I_COMPOSITION_QUIESCENT)) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_callback_gate_destroy(&runtime->lifecycle_gate);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_coordinator_destroy(&runtime->coordinator);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    memset(runtime, 0, storage_bytes);
    return UCN_OK;
}
